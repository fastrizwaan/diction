#include "dict-mmap.h"
#include "flat-index.h"
#include "dict-cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include <libxml/xmlreader.h>
#include <zlib.h>
#include <errno.h>

static int ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return 0;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static char* extract_xdxf_from_archive(const char *archive_path, const char *temp_dir) {
    struct archive *a = archive_read_new();
    struct archive_entry *entry;
    char *extracted_path = NULL;

    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return NULL;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (name && (ends_with_ci(name, ".xdxf") || ends_with_ci(name, ".xdxf.dz"))) {
            const char *base = strrchr(name, '/');
            if (base) base++; else base = name;
            
            extracted_path = g_build_filename(temp_dir, base, NULL);
            int fd = open(extracted_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                archive_read_data_into_fd(a, fd);
                close(fd);
            } else {
                g_free(extracted_path);
                extracted_path = NULL;
            }
            break;
        }
        archive_read_data_skip(a);
    }

    archive_read_close(a);
    archive_read_free(a);
    return extracted_path;
}

static char* decompress_xdxf_dz(const char *dz_path, const char *temp_dir) {
    gzFile gz = gzopen(dz_path, "rb");
    if (!gz) return NULL;

    const char *base = strrchr(dz_path, '/');
    if (base) base++; else base = dz_path;
    char *out_name = g_strndup(base, strlen(base) - 3); // strip .dz
    char *out_path = g_build_filename(temp_dir, out_name, NULL);
    g_free(out_name);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        gzclose(gz);
        g_free(out_path);
        return NULL;
    }

    char buf[65536];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(out);
    gzclose(gz);
    return out_path;
}

typedef struct {
    FILE *out_file;
    GArray *entries;
    uint64_t current_offset;
    char *dict_name;
    char *source_lang;
    char *target_lang;
} XdxfParserState;

static void process_xml_xdxf(xmlTextReaderPtr reader, XdxfParserState *state, volatile gint *cancel_flag, gint expected) {
    int ret = xmlTextReaderRead(reader);
    while (ret == 1) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;

        const xmlChar *name = xmlTextReaderConstLocalName(reader);
        int type = xmlTextReaderNodeType(reader);

        if (type == XML_READER_TYPE_ELEMENT) {
            if (xmlStrEqual(name, (const xmlChar*)"xdxf")) {
                xmlChar *lang_from = xmlTextReaderGetAttribute(reader, (const xmlChar*)"lang_from");
                if (lang_from) {
                    state->source_lang = g_strdup((const char*)lang_from);
                    xmlFree(lang_from);
                }
                xmlChar *lang_to = xmlTextReaderGetAttribute(reader, (const xmlChar*)"lang_to");
                if (lang_to) {
                    state->target_lang = g_strdup((const char*)lang_to);
                    xmlFree(lang_to);
                }
                // Also check if full_name is an attribute (lousy DTD might use it)
                xmlChar *full_name_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"full_name");
                if (full_name_attr && !state->dict_name) {
                    state->dict_name = g_strdup((const char*)full_name_attr);
                    xmlFree(full_name_attr);
                }
            } else if (xmlStrEqual(name, (const xmlChar*)"full_name")) {
                xmlChar *full_name = xmlTextReaderReadString(reader);
                if (full_name && !state->dict_name) {
                    state->dict_name = g_strdup((const char*)full_name);
                    xmlFree(full_name);
                }
            } else if (xmlStrEqual(name, (const xmlChar*)"ar")) {
                int ar_depth = xmlTextReaderDepth(reader);
                GString *hw_str  = g_string_new("");
                GString *def_str = g_string_new("");
                /* Everything outside <dtrn> accumulates here as a label/preamble.
                   When <dtrn> opens we inject it inside the block div. */
                GString *label_buf  = g_string_new("");
                gboolean in_dtrn = FALSE;

                while (xmlTextReaderRead(reader) == 1 && xmlTextReaderDepth(reader) > ar_depth) {
                    const xmlChar *inner_name = xmlTextReaderConstLocalName(reader);
                    int inner_type = xmlTextReaderNodeType(reader);
                    int cur_depth  = xmlTextReaderDepth(reader);

                    /* Which GString gets appended — label before dtrn, def inside dtrn */
                    GString *out = in_dtrn ? def_str : label_buf;

                    /* Suppress structural whitespace outside dtrn */
                    if (!in_dtrn &&
                        (inner_type == XML_READER_TYPE_WHITESPACE ||
                         inner_type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE)) {
                        continue;
                    }

                    if (inner_type == XML_READER_TYPE_ELEMENT) {
                        if (xmlStrEqual(inner_name, (const xmlChar*)"k")) {
                            int k_depth = cur_depth;
                            while (xmlTextReaderRead(reader) == 1 && xmlTextReaderDepth(reader) > k_depth) {
                                if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_TEXT ||
                                    xmlTextReaderNodeType(reader) == XML_READER_TYPE_CDATA) {
                                    const xmlChar *val = xmlTextReaderConstValue(reader);
                                    if (val) {
                                        if (hw_str->len > 0) g_string_append(hw_str, "; ");
                                        g_string_append(hw_str, (const char*)val);
                                    }
                                }
                            }
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"def")) {
                            /* transparent container */
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"dtrn")) {
                            in_dtrn = TRUE;
                            g_string_append(def_str, "<div class=\"trn\">");
                            if (label_buf->len > 0) {
                                g_string_append(def_str, "<span class=\"trn-label\">");
                                g_string_append(def_str, label_buf->str);
                                g_string_append(def_str, "</span>");
                                g_string_truncate(label_buf, 0);
                            }
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"ex")) {
                            g_string_append(out, "<span class=\"ex\">");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"co")) {
                            g_string_append(out, "<span class=\"com\">");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"abr")) {
                            g_string_append(out, "<i>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"tr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"i")) {
                            g_string_append(out, "<i>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"b")) {
                            g_string_append(out, "<b>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"c")) {
                            xmlChar *c_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"c");
                            if (c_attr) {
                                g_string_append_printf(out, "<span style=\"color:%s\">", (const char*)c_attr);
                                xmlFree(c_attr);
                            } else {
                                g_string_append(out, "<span>");
                            }
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref")) {
                            g_string_append(out, "<a class='dict-link' href='#'>");
                        }
                    } else if (inner_type == XML_READER_TYPE_END_ELEMENT) {
                        if (xmlStrEqual(inner_name, (const xmlChar*)"dtrn")) {
                            g_string_append(def_str, "</div>");
                            in_dtrn = FALSE;
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"abr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"tr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"i")) {
                            g_string_append(out, "</i>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"co") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"c") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"ex")) {
                            g_string_append(out, "</span>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"b")) {
                            g_string_append(out, "</b>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref")) {
                            g_string_append(out, "</a>");
                        }
                    } else if (inner_type == XML_READER_TYPE_TEXT ||
                               inner_type == XML_READER_TYPE_CDATA ||
                               inner_type == XML_READER_TYPE_WHITESPACE ||
                               inner_type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE) {
                        const xmlChar *value = xmlTextReaderConstValue(reader);
                        if (value) {
                            char *escaped = g_markup_escape_text((const char*)value, -1);
                            g_string_append(out, escaped);
                            g_free(escaped);
                        }
                    }
                }
                /* If label exists without dtrn, treat it as inline block (Fix Problem 1) */
                if (label_buf->len > 0) {
                    g_string_append(def_str, "<div class=\"trn\">");
                    g_string_append(def_str, "<span class=\"trn-label\">");
                    g_string_append(def_str, label_buf->str);
                    g_string_append(def_str, "</span>");
                    g_string_append(def_str, "</div>");
                }
                g_string_free(label_buf, TRUE);
                
                if (hw_str->len > 0) {
                    TreeEntry entry;
                    entry.h_off = state->current_offset;
                    entry.h_len = hw_str->len;
                    fwrite(hw_str->str, 1, hw_str->len, state->out_file);
                    fputc(0, state->out_file);
                    
                    entry.d_off = entry.h_off + entry.h_len + 1;
                    entry.d_len = def_str->len;
                    fwrite(def_str->str, 1, def_str->len, state->out_file);
                    fputc(0, state->out_file);
                    
                    g_array_append_val(state->entries, entry);
                    state->current_offset += entry.h_len + 1 + entry.d_len + 1;
                }
                g_string_free(hw_str, TRUE);
                g_string_free(def_str, TRUE);
                
                continue;
            }
        }
        ret = xmlTextReaderRead(reader);
    }
}

static void xdxf_save_meta(const char *cache_path, const char *name, const char *slang, const char *tlang) {
    GKeyFile *kf = g_key_file_new();
    if (name) g_key_file_set_string(kf, "Metadata", "Name", name);
    if (slang) g_key_file_set_string(kf, "Metadata", "SourceLang", slang);
    if (tlang) g_key_file_set_string(kf, "Metadata", "TargetLang", tlang);
    
    char *meta_path = g_strdup_printf("%s.meta", cache_path);
    g_key_file_save_to_file(kf, meta_path, NULL);
    g_free(meta_path);
    g_key_file_free(kf);
}

static void xdxf_load_meta(const char *cache_path, char **name, char **slang, char **tlang) {
    char *meta_path = g_strdup_printf("%s.meta", cache_path);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, meta_path, G_KEY_FILE_NONE, NULL)) {
        *name = g_key_file_get_string(kf, "Metadata", "Name", NULL);
        *slang = g_key_file_get_string(kf, "Metadata", "SourceLang", NULL);
        *tlang = g_key_file_get_string(kf, "Metadata", "TargetLang", NULL);
    }
    g_key_file_free(kf);
    g_free(meta_path);
}

DictMmap* parse_xdxf_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;

    dict_cache_ensure_dir();
    char *cache_path = dict_cache_path_for(path);
    if (dict_cache_is_valid(cache_path, path)) {
        int fd = open(cache_path, O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            fstat(fd, &st);
            size_t size = st.st_size;
            const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (data != MAP_FAILED) {
                DictMmap *dict = g_new0(DictMmap, 1);
                dict->fd = fd;
                dict->data = data;
                dict->size = size;
                xdxf_load_meta(cache_path, &dict->name, &dict->source_lang, &dict->target_lang);
                dict->index = flat_index_open(data, size);
                g_free(cache_path);
                return dict;
            }
            close(fd);
        }
    }

    // Need to build cache
    char *temp_dir = g_dir_make_tmp("diction-xdxf-XXXXXX", NULL);
    char *xml_path = NULL;

    if (ends_with_ci(path, ".tar.bz2") || ends_with_ci(path, ".tar.gz") || ends_with_ci(path, ".tar.xz") || ends_with_ci(path, ".tgz")) {
        xml_path = extract_xdxf_from_archive(path, temp_dir);
    } else if (ends_with_ci(path, ".xdxf.dz")) {
        xml_path = decompress_xdxf_dz(path, temp_dir);
    } else {
        xml_path = g_strdup(path);
    }

    if (!xml_path) {
        g_rmdir(temp_dir);
        g_free(temp_dir);
        g_free(cache_path);
        return NULL;
    }

    // Now if xml_path is still .dz (e.g. from archive), decompress it
    if (ends_with_ci(xml_path, ".xdxf.dz")) {
        char *new_xml_path = decompress_xdxf_dz(xml_path, temp_dir);
        if (xml_path != path) {
            unlink(xml_path);
            g_free(xml_path);
        }
        xml_path = new_xml_path;
    }

    if (!xml_path) {
        g_rmdir(temp_dir);
        g_free(temp_dir);
        g_free(cache_path);
        return NULL;
    }

    xmlTextReaderPtr reader = xmlNewTextReaderFilename(xml_path);
    if (!reader) {
        if (xml_path != path) { unlink(xml_path); g_free(xml_path); }
        g_rmdir(temp_dir);
        g_free(temp_dir);
        g_free(cache_path);
        return NULL;
    }

    FILE *cache_file = fopen(cache_path, "wb");
    uint64_t zero_count = 0;
    fwrite(&zero_count, 8, 1, cache_file);

    XdxfParserState state = {0};
    state.out_file = cache_file;
    state.entries = g_array_new(FALSE, TRUE, sizeof(TreeEntry));
    state.current_offset = 8;

    process_xml_xdxf(reader, &state, cancel_flag, expected);

    xmlFreeTextReader(reader);
    if (xml_path != path) { unlink(xml_path); g_free(xml_path); }
    g_rmdir(temp_dir);
    g_free(temp_dir);

    if (state.entries->len > 0) {
        fflush(cache_file);
        long data_end = ftell(cache_file);
        
        // Map back to sort
        int tmp_fd = open(cache_path, O_RDONLY);
        const char *tmp_data = mmap(NULL, data_end, PROT_READ, MAP_PRIVATE, tmp_fd, 0);
        
        flat_index_sort_entries((TreeEntry*)state.entries->data, state.entries->len, tmp_data, data_end);
        
        munmap((void*)tmp_data, data_end);
        close(tmp_fd);
        
        fseek(cache_file, data_end, SEEK_SET);
        fwrite(state.entries->data, sizeof(TreeEntry), state.entries->len, cache_file);
        fseek(cache_file, 0, SEEK_SET);
        uint64_t count = state.entries->len;
        fwrite(&count, 8, 1, cache_file);
    }
    fclose(cache_file);

    xdxf_save_meta(cache_path, state.dict_name, state.source_lang, state.target_lang);

    // Sync mtime
    const char *sources[] = {path};
    dict_cache_sync_mtime(cache_path, sources, 1);

    // Finally open the cache
    int fd = open(cache_path, O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    size_t size = st.st_size;
    const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = fd;
    dict->data = data;
    dict->size = size;
    dict->name = state.dict_name;
    dict->source_lang = state.source_lang;
    dict->target_lang = state.target_lang;
    dict->index = flat_index_open(data, size);

    g_array_free(state.entries, TRUE);
    g_free(cache_path);

    return dict;
}