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
#include "settings.h"

static int ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return 0;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static char* extract_xdxf_xml_from_archive(const char *archive_path, const char *target_dir) {
    settings_scan_progress_notify(archive_path, 5);
    struct archive *a = archive_read_new();
    struct archive_entry *entry;
    char *first_xdxf_path = NULL;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return NULL;
    }

    g_mkdir_with_parents(target_dir, 0755);

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (!name) {
            archive_read_data_skip(a);
            continue;
        }

        if (archive_entry_filetype(entry) == AE_IFDIR) {
            archive_read_data_skip(a);
            continue;
        }

        /* Only extract the .xdxf or .xdxf.dz file, skip all media */
        if (ends_with_ci(name, ".xdxf") || ends_with_ci(name, ".xdxf.dz")) {
            char *extracted_path = g_build_filename(target_dir, name, NULL);
            
            /* Create parent directories if needed */
            char *dirname = g_path_get_dirname(extracted_path);
            g_mkdir_with_parents(dirname, 0755);
            g_free(dirname);

            first_xdxf_path = g_strdup(extracted_path);

            la_int64_t entry_size = archive_entry_size(entry);
            guint64 bytes_needed = entry_size > 0 ? (guint64) entry_size : 0;
            if (!dict_cache_prepare_target_path(extracted_path, bytes_needed)) {
                g_free(first_xdxf_path);
                first_xdxf_path = NULL;
                g_free(extracted_path);
                break;
            }

            int fd = open(extracted_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                archive_read_data_into_fd(a, fd);
                close(fd);
                settings_scan_progress_notify(archive_path, 15);
            }
            g_free(extracted_path);
            
            /* Stop reading the archive once we've found and extracted the XDXF file */
            break;
        } else {
            archive_read_data_skip(a);
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    settings_scan_progress_notify(archive_path, 30);
    return first_xdxf_path;
}

static char* decompress_xdxf_dz(const char *dz_path, const char *temp_dir) {
    gzFile gz = gzopen(dz_path, "rb");
    if (!gz) return NULL;
    settings_scan_progress_notify(dz_path, 5);

    const char *base = strrchr(dz_path, '/');
    if (base) base++; else base = dz_path;
    char *out_name = g_strndup(base, strlen(base) - 3); // strip .dz
    char *out_path = g_build_filename(temp_dir, out_name, NULL);
    g_free(out_name);

    struct stat st;
    guint64 bytes_needed = (stat(dz_path, &st) == 0 && st.st_size > 0)
        ? (guint64) st.st_size
        : 0;
    if (!dict_cache_prepare_target_path(out_path, bytes_needed)) {
        gzclose(gz);
        g_free(out_path);
        return NULL;
    }

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
    settings_scan_progress_notify(dz_path, 30);
    return out_path;
}

typedef struct {
    FILE *out_file;
    GArray *entries;
    uint64_t current_offset;
    char *dict_name;
    char *source_lang;
    char *target_lang;
    char *resource_dir;
} XdxfParserState;

static void process_xml_xdxf(xmlTextReaderPtr reader, XdxfParserState *state, const char *path, volatile gint *cancel_flag, gint expected) {
    int ret = xmlTextReaderRead(reader);
    int total_ars = 0;
    while (ret == 1) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;

        const xmlChar *name = xmlTextReaderConstLocalName(reader);
        int type = xmlTextReaderNodeType(reader);
        
        if (type == XML_READER_TYPE_ELEMENT && xmlStrEqual(name, (const xmlChar*)"ar")) {
            total_ars++;
            if (total_ars % 100 == 0) {
                /* We don't know total ARs, so we use a heuristic or just incrementing progress if possible.
                 * Actually, let's use byte offset from the reader if available. */
                int64_t offset = xmlTextReaderByteConsumed(reader);
                /* We don't have total size here easily, but we can assume some progress. */
                settings_scan_progress_notify(path, 30 + (total_ars / 500) % 65);
            }
        }

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
                xmlChar *full_name_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"full_name");
                if (full_name_attr && !state->dict_name) {
                    state->dict_name = g_strdup((const char*)full_name_attr);
                    xmlFree(full_name_attr);
                }
            } else if (xmlStrEqual(name, (const xmlChar*)"full_name") || 
                       xmlStrEqual(name, (const xmlChar*)"title") ||
                       xmlStrEqual(name, (const xmlChar*)"description")) {
                xmlChar *val = xmlTextReaderReadString(reader);
                if (val && !state->dict_name) {
                    state->dict_name = g_strdup((const char*)val);
                    xmlFree(val);
                }
            }
 else if (xmlStrEqual(name, (const xmlChar*)"ar")) {
                int ar_depth = xmlTextReaderDepth(reader);
                GString *hw_str  = g_string_new("");
                // Wrap the article in a semantic container
                GString *def_str = g_string_new("<div class=\"dictionary-entry xdxf-ar\">");
                int list_depth = 0;

                while (xmlTextReaderRead(reader) == 1 && xmlTextReaderDepth(reader) > ar_depth) {
                    const xmlChar *inner_name = xmlTextReaderConstLocalName(reader);
                    int inner_type = xmlTextReaderNodeType(reader);
                    int cur_depth  = xmlTextReaderDepth(reader);

                    if (inner_type == XML_READER_TYPE_ELEMENT) {
                        gboolean is_empty = xmlTextReaderIsEmptyElement(reader);
                        if (!is_empty && (xmlStrEqual(inner_name, (const xmlChar*)"ol") ||
                                          xmlStrEqual(inner_name, (const xmlChar*)"ul") ||
                                          xmlStrEqual(inner_name, (const xmlChar*)"table") ||
                                          xmlStrEqual(inner_name, (const xmlChar*)"tr") ||
                                          xmlStrEqual(inner_name, (const xmlChar*)"tbody") ||
                                          xmlStrEqual(inner_name, (const xmlChar*)"thead"))) {
                            list_depth++;
                        }
                        
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
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"a")) {
                            xmlChar *href_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"href");
                            if (href_attr) {
                                g_string_append_printf(def_str, "<a class=\"xdxf-a\" href=\"%s\">", (const char*)href_attr);
                                xmlFree(href_attr);
                            } else {
                                g_string_append(def_str, "<a class=\"xdxf-a\">");
                            }
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"b") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"i") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"u") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"sub") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"sup") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"ul") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"ol") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"li") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"p") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"div") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"span") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"table") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"tr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"td") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"th") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"br") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"hr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"blockquote")) {
                            // Preserve native formatting tags but attach the class
                            g_string_append_printf(def_str, "<%s class=\"xdxf-%s\">", (const char*)inner_name, (const char*)inner_name);
                            
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"c")) {
                            // Map explicit color definitions
                            xmlChar *c_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"c");
                            if (c_attr) {
                                g_string_append_printf(def_str, "<span class=\"xdxf-c\" style=\"color: %s;\">", (const char*)c_attr);
                                xmlFree(c_attr);
                            } else {
                                g_string_append(def_str, "<span class=\"xdxf-c\">");
                            }
                            
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref")) {
                            // Map dictionary cross-reference links
                            xmlChar *k_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"k");
                            if (k_attr) {
                                char *uri_attr = g_uri_escape_string((const char*)k_attr, NULL, TRUE);
                                g_string_append_printf(def_str, "<a href=\"dict://%s\" class=\"xdxf-kref\">", uri_attr);
                                g_free(uri_attr);
                                xmlFree(k_attr);
                            } else {
                                // If 'k' is missing, use the text content as the link target
                                if (xmlTextReaderExpand(reader) >= 0) {
                                    xmlNodePtr node = xmlTextReaderCurrentNode(reader);
                                    xmlChar *val = xmlNodeGetContent(node);
                                    if (val) {
                                        char *uri_word = g_uri_escape_string((const char*)val, NULL, TRUE);
                                        g_string_append_printf(def_str, "<a href=\"dict://%s\" class=\"xdxf-kref\">", uri_word);
                                        g_free(uri_word);
                                        xmlFree(val);
                                    } else {
                                        g_string_append(def_str, "<a href=\"dict://\" class=\"xdxf-kref\">");
                                    }
                                } else {
                                    g_string_append(def_str, "<a href=\"dict://\" class=\"xdxf-kref\">");
                                }
                            }
                            
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"def")) {
                            // Definition blocks become inline to allow explicit \n breaks
                            g_string_append(def_str, "<span class=\"xdxf-def\">");
                            
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"img")) {
                            xmlChar *src = xmlTextReaderGetAttribute(reader, (const xmlChar*)"src");
                            if (src) {
                                g_string_append_printf(def_str, "<img class=\"xdxf-img\" src=\"%s\" />", (const char*)src);
                                xmlFree(src);
                            }
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"snd")) {
                            xmlChar *src = xmlTextReaderGetAttribute(reader, (const xmlChar*)"src");
                            if (src) {
                                g_string_append_printf(def_str, "<a class=\"xdxf-snd\" href=\"sound://%s\">🔊</a>", (const char*)src);
                                xmlFree(src);
                            }
                        } else {
                            // Everything else (dtrn, ex, co, abr, tr) maps dynamically to semantic span elements
                            g_string_append_printf(def_str, "<span class=\"xdxf-%s\">", (const char*)inner_name);
                        }
                    } else if (inner_type == XML_READER_TYPE_END_ELEMENT) {
                        if (xmlStrEqual(inner_name, (const xmlChar*)"ol") ||
                            xmlStrEqual(inner_name, (const xmlChar*)"ul") ||
                            xmlStrEqual(inner_name, (const xmlChar*)"table") ||
                            xmlStrEqual(inner_name, (const xmlChar*)"tr") ||
                            xmlStrEqual(inner_name, (const xmlChar*)"tbody") ||
                            xmlStrEqual(inner_name, (const xmlChar*)"thead")) {
                            if (list_depth > 0) list_depth--;
                        }

                        if (xmlStrEqual(inner_name, (const xmlChar*)"k")) {
                            // Handled natively by inner sub-loop
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"b") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"i") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"u") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"sub") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"sup") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"ul") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"ol") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"li") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"p") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"div") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"span") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"table") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"tr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"td") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"th") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"br") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"hr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"blockquote")) {
                            g_string_append_printf(def_str, "</%s>", (const char*)inner_name);
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref") || xmlStrEqual(inner_name, (const xmlChar*)"a")) {
                            g_string_append(def_str, "</a>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"def")) {
                            g_string_append(def_str, "</span>");
                        } else {
                            g_string_append(def_str, "</span>");
                        }
                    } else if (inner_type == XML_READER_TYPE_TEXT ||
                               inner_type == XML_READER_TYPE_CDATA ||
                               inner_type == XML_READER_TYPE_WHITESPACE ||
                               inner_type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE) {
                        const xmlChar *value = xmlTextReaderConstValue(reader);
                        if (value) {
                            const char *ptr = (const char*)value;
                            const char *start = ptr;
                            gboolean is_line_start = TRUE;
                            
                            while (*ptr) {
                                if (*ptr == '\n' || *ptr == '\t' || *ptr == ' ') {
                                    if (ptr > start) {
                                        char *esc = g_markup_escape_text(start, (gssize)(ptr - start));
                                        g_string_append(def_str, esc);
                                        g_free(esc);
                                        is_line_start = FALSE;
                                    }
                                    
                                    if (*ptr == '\n') {
                                        if (list_depth == 0) {
                                            g_string_append(def_str, "<br/>");
                                        } else {
                                            g_string_append_c(def_str, '\n');
                                        }
                                        is_line_start = TRUE;
                                    } else if (*ptr == '\t') {
                                        g_string_append(def_str, "&nbsp;&nbsp;&nbsp;&nbsp;");
                                        is_line_start = FALSE;
                                    } else if (*ptr == ' ') {
                                        if (is_line_start) {
                                            g_string_append(def_str, "&nbsp;");
                                        } else {
                                            g_string_append_c(def_str, ' ');
                                        }
                                    }
                                    
                                    ptr++;
                                    start = ptr;
                                } else {
                                    ptr = g_utf8_next_char(ptr);
                                }
                            }
                            
                            if (ptr > start) {
                                char *esc = g_markup_escape_text(start, (gssize)(ptr - start));
                                g_string_append(def_str, esc);
                                g_free(esc);
                            }
                        }
                    }
                }
                
                g_string_append(def_str, "</div>");
                
                // Write payload to index
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
    settings_scan_progress_notify(path, 95);
}
static void xdxf_save_meta(const char *cache_path, const char *name, const char *slang, const char *tlang, const char *archive_path) {
    GKeyFile *kf = g_key_file_new();
    if (name) g_key_file_set_string(kf, "Metadata", "Name", name);
    if (slang) g_key_file_set_string(kf, "Metadata", "SourceLang", slang);
    if (tlang) g_key_file_set_string(kf, "Metadata", "TargetLang", tlang);
    if (archive_path) g_key_file_set_string(kf, "Metadata", "Archive", archive_path);
    
    char *meta_path = g_strdup_printf("%s.meta", cache_path);
    g_key_file_save_to_file(kf, meta_path, NULL);
    g_free(meta_path);
    g_key_file_free(kf);
}

static void xdxf_load_meta(const char *cache_path, char **name, char **slang, char **tlang, char **archive_path) {
    char *meta_path = g_strdup_printf("%s.meta", cache_path);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, meta_path, G_KEY_FILE_NONE, NULL)) {
        *name = g_key_file_get_string(kf, "Metadata", "Name", NULL);
        *slang = g_key_file_get_string(kf, "Metadata", "SourceLang", NULL);
        *tlang = g_key_file_get_string(kf, "Metadata", "TargetLang", NULL);
        *archive_path = g_key_file_get_string(kf, "Metadata", "Archive", NULL);
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
                char *archive_path = NULL;
                xdxf_load_meta(cache_path, &dict->name, &dict->source_lang, &dict->target_lang, &archive_path);
                
                if (archive_path) {
                    char *res_dir = g_strdup_printf("%s.res", cache_path);
                    dict->resource_reader = resource_reader_open_archive(archive_path, res_dir);
                    dict->resource_dir = res_dir; /* Takes ownership */
                } else {
                    char *res_dir = g_strdup_printf("%s.res", cache_path);
                    if (g_file_test(res_dir, G_FILE_TEST_IS_DIR)) {
                        dict->resource_dir = res_dir;
                    } else {
                        g_free(res_dir);
                    }
                }
                g_free(archive_path);

                dict->source_dir = g_canonicalize_filename(g_path_get_dirname(path), NULL);
                dict->index = flat_index_open(data, size);
                g_free(cache_path);
                return dict;
            }
            close(fd);
        }
    }

    // Need to build cache
    char *res_dir = g_strdup_printf("%s.res", cache_path);
    char *xml_path = NULL;

    if (ends_with_ci(path, ".tar.bz2") || ends_with_ci(path, ".tar.gz") || ends_with_ci(path, ".tar.xz") || ends_with_ci(path, ".tgz") || ends_with_ci(path, ".zip")) {
        xml_path = extract_xdxf_xml_from_archive(path, res_dir);
    } else if (ends_with_ci(path, ".xdxf.dz")) {
        /* Decompress into res_dir to keep it persistent if needed, or just temp */
        g_mkdir_with_parents(res_dir, 0755);
        xml_path = decompress_xdxf_dz(path, res_dir);
    } else {
        xml_path = g_strdup(path);
    }

    if (!xml_path) {
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }

    // Now if xml_path is still .dz (e.g. from archive), decompress it
    if (ends_with_ci(xml_path, ".xdxf.dz")) {
        char *temp_dir = g_path_get_dirname(xml_path);
        char *new_xml_path = decompress_xdxf_dz(xml_path, temp_dir);
        if (xml_path != path) {
            unlink(xml_path);
            g_free(xml_path);
        }
        xml_path = new_xml_path;
        g_free(temp_dir);
    }

    if (!xml_path) {
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }

    xmlTextReaderPtr reader = xmlNewTextReaderFilename(xml_path);
    if (!reader) {
        if (xml_path != path) { unlink(xml_path); g_free(xml_path); }
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }

    struct stat cache_src_st;
    guint64 cache_bytes_hint = (stat(path, &cache_src_st) == 0 && cache_src_st.st_size > 0)
        ? (guint64) cache_src_st.st_size
        : 0;
    if (!dict_cache_prepare_target_path(cache_path, cache_bytes_hint)) {
        xmlFreeTextReader(reader);
        if (xml_path != path) { unlink(xml_path); g_free(xml_path); }
        g_free(res_dir);
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
    state.resource_dir = res_dir;

    process_xml_xdxf(reader, &state, path, cancel_flag, expected);

    xmlFreeTextReader(reader);
    if (xml_path != path) {
        /* If it was extracted from archive, it's in res_dir. We might want to keep it? 
         * Actually, we only need the indexed data for dictionary content, 
         * BUT the user might want resources. 
         * We keep everything in res_dir. */
    }

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

    const char *archive_path = NULL;
    if (ends_with_ci(path, ".tar.bz2") || ends_with_ci(path, ".tar.gz") || ends_with_ci(path, ".tar.xz") || ends_with_ci(path, ".tgz") || ends_with_ci(path, ".zip")) {
        archive_path = path;
    }
    xdxf_save_meta(cache_path, state.dict_name, state.source_lang, state.target_lang, archive_path);

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
    
    // Set resource directory if we extracted an archive
    if (archive_path) {
        dict->resource_reader = resource_reader_open_archive(path, res_dir);
        dict->resource_dir = g_strdup(res_dir);
    } else if (g_file_test(res_dir, G_FILE_TEST_IS_DIR)) {
        dict->resource_dir = g_strdup(res_dir);
    }
    
    dict->source_dir = g_canonicalize_filename(g_path_get_dirname(path), NULL);
    g_free(res_dir);

    g_array_free(state.entries, TRUE);
    g_free(cache_path);

    settings_scan_progress_notify(path, 100);
    return dict;
}
