
#include "dict-mmap.h"
#include "flat-index.h"
#include "dict-cache.h"
#include "dict-cache-builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include <libxml/xmlreader.h>
#include <zlib.h>
#include <errno.h>
#include "settings.h"
#include "dictzip.h"

static int ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return 0;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static char* extract_xdxf_xml_from_archive(const char *archive_path, const char *target_dir, volatile gint *cancel_flag, gint expected) {
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
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
            break;
        }
        const char *name = archive_entry_pathname(entry);
        if (!name) {
            archive_read_data_skip(a);
            continue;
        }
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            archive_read_data_skip(a);
            continue;
        }

        if (ends_with_ci(name, ".xdxf") || ends_with_ci(name, ".xdxf.dz")) {
            char *extracted_path = g_build_filename(target_dir, name, NULL);
            char *dirname = g_path_get_dirname(extracted_path);
            g_mkdir_with_parents(dirname, 0755);
            g_free(dirname);
            first_xdxf_path = g_strdup(extracted_path);
            la_int64_t entry_size = archive_entry_size(entry);
            guint64 bytes_needed = entry_size > 0 ? (guint64) entry_size : 0;
            if (!dict_cache_prepare_target_path(extracted_path, bytes_needed)) {
                g_free(first_xdxf_path); first_xdxf_path = NULL;
                g_free(extracted_path); break;
            }
            int fd = open(extracted_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                char buffer[32768];
                int bytes_read;
                while ((bytes_read = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
                    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                        close(fd);
                        unlink(extracted_path);
                        g_free(extracted_path);
                        if (first_xdxf_path) { g_free(first_xdxf_path); first_xdxf_path = NULL; }
                        archive_read_close(a);
                        archive_read_free(a);
                        return NULL;
                    }
                    write(fd, buffer, bytes_read);
                }
                close(fd);
                settings_scan_progress_notify(archive_path, 15);
            }
            g_free(extracted_path);
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

static char* decompress_xdxf_dz(const char *dz_path, const char *temp_dir, volatile gint *cancel_flag, gint expected) {
    gzFile gz = gzopen(dz_path, "rb");
    if (!gz) return NULL;
    settings_scan_progress_notify(dz_path, 5);

    const char *base = strrchr(dz_path, '/');
    if (base) base++; else base = dz_path;
    char *out_name = g_strndup(base, strlen(base) - 3);
    char *out_path = g_build_filename(temp_dir, out_name, NULL);
    g_free(out_name);

    struct stat st;
    guint64 bytes_needed = (stat(dz_path, &st) == 0 && st.st_size > 0) ? (guint64) st.st_size : 0;
    if (!dict_cache_prepare_target_path(out_path, bytes_needed)) {
        gzclose(gz); g_free(out_path); return NULL;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        gzclose(gz); g_free(out_path); return NULL;
    }

    char buf[65536]; int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
            fclose(out); gzclose(gz);
            unlink(out_path);
            g_free(out_path);
            return NULL;
        }
        fwrite(buf, 1, n, out);
    }
    fclose(out); gzclose(gz);
    settings_scan_progress_notify(dz_path, 30);
    return out_path;
}

enum { XDXF_STANDARD_STRICT = 0, XDXF_STANDARD_LOUSY = 1 };
enum { XDXF_LOUSY_FORMAT_UNKNOWN = 0, XDXF_LOUSY_FORMAT_LOGICAL = 1, XDXF_LOUSY_FORMAT_VISUAL = 2 };

static int xdxf_detect_standard_from_file(const char *xml_path) {
    if (!xml_path) return XDXF_STANDARD_STRICT;
    FILE *fp = fopen(xml_path, "rb");
    if (!fp) return XDXF_STANDARD_STRICT;
    char sniff[8193];
    size_t n = fread(sniff, 1, sizeof(sniff) - 1, fp);
    fclose(fp);
    sniff[n] = '\0';
    if (g_strstr_len(sniff, (gssize)n, "xdxf_lousy.dtd") != NULL) {
        return XDXF_STANDARD_LOUSY;
    }
    return XDXF_STANDARD_STRICT;
}

static const char *xdxf_profile_class(int standard) {
    return (standard == XDXF_STANDARD_LOUSY) ? "xdxf-profile-lousy" : "xdxf-profile-strict";
}

static const char *xdxf_lousy_format_class(int format_kind) {
    if (format_kind == XDXF_LOUSY_FORMAT_VISUAL) return "xdxf-format-visual";
    if (format_kind == XDXF_LOUSY_FORMAT_LOGICAL) return "xdxf-format-logical";
    return "xdxf-format-unknown";
}

static gboolean xdxf_is_number_marker_text(const char *text) {
    if (!text) return FALSE;
    const char *p = text;
    while (*p && g_ascii_isspace(*p)) p++;
    if (!g_ascii_isdigit(*p)) return FALSE;
    while (*p && g_ascii_isdigit(*p)) p++;
    return (*p == '.');
}

static gboolean xdxf_is_roman_marker_text(const char *text) {
    if (!text) return FALSE;
    const char *p = text;
    while (*p && g_ascii_isspace(*p)) p++;
    if (!*p) return FALSE;
    const char *start = p;
    while (*p) {
        char ch = *p;
        if (!(ch == 'I' || ch == 'V' || ch == 'X' || ch == 'L' || ch == 'C' || ch == 'D' || ch == 'M')) break;
        p++;
    }
    if (p == start) return FALSE;
    while (*p && g_ascii_isspace(*p)) p++;
    return (*p == '\0');
}

static gboolean xdxf_is_letter_marker_text(const char *text) {
    if (!text) return FALSE;
    const char *p = text;
    while (*p && g_ascii_isspace(*p)) p++;
    if (!g_ascii_isalpha(*p)) return FALSE;
    p++;
    return (*p == '.');
}

static char *xdxf_collapse_whitespace(const char *text, gboolean trim_edges) {
    if (!text) return g_strdup("");
    GString *collapsed = g_string_new("");
    gboolean in_ws = FALSE;
    for (const char *p = text; *p; ) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isspace(ch)) {
            if (!in_ws) {
                g_string_append_c(collapsed, ' ');
                in_ws = TRUE;
            }
        } else {
            char utf8[7] = {0};
            gint len = g_unichar_to_utf8(ch, utf8);
            utf8[len] = '\0';
            g_string_append(collapsed, utf8);
            in_ws = FALSE;
        }
        p = g_utf8_next_char(p);
    }
    if (trim_edges) {
        g_strstrip(collapsed->str);
        collapsed->len = strlen(collapsed->str);
    }
    return g_string_free(collapsed, FALSE);
}

static void xdxf_append_escaped_text(GString *out, const char *text, gboolean trim_edges, gboolean preserve_ws) {
    if (!out || !text || !*text) return;
    char *processed = NULL;
    if (preserve_ws) {
        processed = g_strdup(text);
        if (trim_edges) g_strstrip(processed);
    } else {
        processed = xdxf_collapse_whitespace(text, trim_edges);
    }
    if (!processed || !*processed) { g_free(processed); return; }
    if (!preserve_ws) {
        if (out->len == 0) g_strchug(processed);
        else if (g_ascii_isspace(out->str[out->len - 1])) g_strchug(processed);
    }
    if (!*processed) { g_free(processed); return; }
    char *escaped = g_markup_escape_text(processed, -1);
    g_string_append(out, escaped);
    g_free(escaped); g_free(processed);
}

static void xdxf_append_space_if_needed(GString *out) {
    if (!out || out->len == 0) return;
    if (g_ascii_isspace(out->str[out->len - 1])) return;
    g_string_append_c(out, ' ');
}

static void xdxf_flush_pending_space(GString *out, gboolean *pending_space, gboolean has_inline_content, gboolean preserve_ws) {
    if (!pending_space || !*pending_space) return;
    if (preserve_ws) {
        g_string_append_c(out, ' '); // just flush a single space if pending, though preserve_ws usually doesn't have pending spaces.
    } else {
        if (has_inline_content) xdxf_append_space_if_needed(out);
    }
    *pending_space = FALSE;
}

const char* xdxf_get_definition_on_the_fly(DictMmap *dict, const FlatTreeEntry *entry, size_t *out_len, char **out_to_free) {
    if (!dict || !entry) return NULL;
    
    // Read the <ar>...</ar> block from source
    char *raw = NULL;
    size_t raw_len = 0;
    if (dict->source_mmap) {
        raw_len = entry->d_len;
        raw = g_strndup(dict->source_mmap + entry->d_off, raw_len);
    } else if (dict->source_dz) {
        raw = (char*)dictzip_read(dict->source_dz, entry->d_off, entry->d_len, &raw_len);
    }
    if (!raw) return NULL;

    xmlTextReaderPtr reader = xmlReaderForMemory(raw, raw_len, "noname.xml", NULL, XML_PARSE_HUGE | XML_PARSE_RECOVER);
    if (!reader) { g_free(raw); return NULL; }

    GString *def_str = g_string_new("");
    int def_nesting = 0;
    gboolean pending_space = FALSE;
    gboolean has_inline_content = FALSE;
    gboolean in_rref = FALSE;
    int ar_lousy_format = dict->xdxf_lousy_format;
    gboolean apply_special_formatting = (dict->xdxf_standard == XDXF_STANDARD_LOUSY && ar_lousy_format == XDXF_LOUSY_FORMAT_VISUAL);

    // Start wrapper
    g_string_append_printf(def_str, "<div class=\"dictionary-entry xdxf-ar %s %s\">",
                           xdxf_profile_class(dict->xdxf_standard),
                           xdxf_lousy_format_class(ar_lousy_format));

    while (xmlTextReaderRead(reader) == 1) {
        const xmlChar *inner_name = xmlTextReaderConstLocalName(reader);
        int inner_type = xmlTextReaderNodeType(reader);
        if (!inner_name) continue;

        if (inner_type == XML_READER_TYPE_ELEMENT) {
            if (xmlStrEqual(inner_name, (const xmlChar*)"ar")) {
                xmlChar *ar_format = xmlTextReaderGetAttribute(reader, (const xmlChar*)"f");
                if (dict->xdxf_standard == XDXF_STANDARD_LOUSY && ar_format) {
                    if (ar_format[0] == 'v' || ar_format[0] == 'V') ar_lousy_format = XDXF_LOUSY_FORMAT_VISUAL;
                    else if (ar_format[0] == 'l' || ar_format[0] == 'L') ar_lousy_format = XDXF_LOUSY_FORMAT_LOGICAL;
                }
                if (ar_format) xmlFree(ar_format);
                // Update wrapper class if we found a specific format on <ar>
                // We'll just leave the wrapper as initialized with default, 
                // since we already wrote the opening div tag. This is okay for now.
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"def")) {
                pending_space = FALSE; has_inline_content = FALSE; def_nesting++;
                g_string_append_printf(def_str, "<div class=\"xdxf-def xdxf-def-lvl-%d\">", def_nesting);
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"k")) {
                int k_depth = xmlTextReaderDepth(reader);
                while (xmlTextReaderRead(reader) == 1 && xmlTextReaderDepth(reader) > k_depth) {}
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"a")) {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                xmlChar *href_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"href");
                if (href_attr) {
                    g_string_append_printf(def_str, "<a class=\"xdxf-a\" href=\"%s\">", (const char*)href_attr);
                    xmlFree(href_attr);
                } else g_string_append(def_str, "<a class=\"xdxf-a\">");
                has_inline_content = TRUE;
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"b") || xmlStrEqual(inner_name, (const xmlChar*)"i") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"u") || xmlStrEqual(inner_name, (const xmlChar*)"sub") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"sup") || xmlStrEqual(inner_name, (const xmlChar*)"ul") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"ol") || xmlStrEqual(inner_name, (const xmlChar*)"li") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"p") || xmlStrEqual(inner_name, (const xmlChar*)"div") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"span") || xmlStrEqual(inner_name, (const xmlChar*)"br") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"hr") || xmlStrEqual(inner_name, (const xmlChar*)"blockquote")) {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                g_string_append_printf(def_str, "<%s class=\"xdxf-%s\">", (const char*)inner_name, (const char*)inner_name);
                has_inline_content = TRUE;
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"c")) {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                xmlChar *c_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"c");
                if (c_attr) {
                    const char *c_attr_str = (const char*)c_attr;
                    if (apply_special_formatting) {
                        gboolean is_numeric_marker = FALSE;
                        gboolean is_roman_marker = FALSE;
                        gboolean is_letter_marker = FALSE;
                        xmlNodePtr c_node = xmlTextReaderExpand(reader);
                        if (c_node) {
                            xmlChar *c_val = xmlNodeGetContent(c_node);
                            if (c_val) {
                                is_numeric_marker = xdxf_is_number_marker_text((const char*)c_val);
                                if (!is_numeric_marker) is_roman_marker = xdxf_is_roman_marker_text((const char*)c_val);
                                if (!is_numeric_marker && !is_roman_marker) is_letter_marker = xdxf_is_letter_marker_text((const char*)c_val);
                                xmlFree(c_val);
                            }
                        }
                        if (is_numeric_marker) {
                            g_string_append_printf(def_str, "<span class=\"xdxf-c xdxf-c-num\" style=\"color: %s;\">", c_attr_str);
                        } else if (is_roman_marker) {
                            g_string_append_printf(def_str, "<span class=\"xdxf-c xdxf-c-roman\" style=\"color: %s;\">", c_attr_str);
                        } else if (is_letter_marker) {
                            g_string_append_printf(def_str, "<span class=\"xdxf-c xdxf-c-letter\" style=\"color: %s;\">", c_attr_str);
                        } else {
                            g_string_append_printf(def_str, "<span class=\"xdxf-c\" style=\"color: %s;\">", c_attr_str);
                        }
                    } else {
                        g_string_append_printf(def_str, "<span class=\"xdxf-c\" style=\"color: %s;\">", c_attr_str);
                    }
                    xmlFree(c_attr);
                } else g_string_append(def_str, "<span class=\"xdxf-c\">");
                has_inline_content = TRUE;
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref")) {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                xmlChar *k_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"k");
                if (k_attr) {
                    char *uri_attr = g_uri_escape_string((const char*)k_attr, NULL, TRUE);
                    g_string_append_printf(def_str, "<a href=\"dict://%s\" class=\"xdxf-kref\">", uri_attr);
                    g_free(uri_attr); xmlFree(k_attr);
                } else {
                    if (xmlTextReaderExpand(reader) != NULL) {
                        xmlNodePtr node = xmlTextReaderCurrentNode(reader);
                        xmlChar *val = xmlNodeGetContent(node);
                        if (val) {
                            char *uri_word = g_uri_escape_string((const char*)val, NULL, TRUE);
                            g_string_append_printf(def_str, "<a href=\"dict://%s\" class=\"xdxf-kref\">", uri_word);
                            g_free(uri_word); xmlFree(val);
                        } else g_string_append(def_str, "<a href=\"dict://\" class=\"xdxf-kref\">");
                    } else g_string_append(def_str, "<a href=\"dict://\" class=\"xdxf-kref\">");
                }
                has_inline_content = TRUE;
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"iref")) {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                xmlChar *href = xmlTextReaderGetAttribute(reader, (const xmlChar*)"href");
                if (href) {
                    g_string_append_printf(def_str, "<a class=\"xdxf-iref\" href=\"%s\">", (const char*)href);
                    xmlFree(href);
                } else g_string_append(def_str, "<a class=\"xdxf-iref\">");
                has_inline_content = TRUE;
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"rref")) {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                if (has_inline_content) xdxf_append_space_if_needed(def_str);
                xmlChar *lctn = xmlTextReaderGetAttribute(reader, (const xmlChar*)"lctn");
                if (lctn) {
                    const char *l = (const char*)lctn;
                    if (g_str_has_suffix(l, ".ogg") || g_str_has_suffix(l, ".wav") || g_str_has_suffix(l, ".mp3") || g_str_has_suffix(l, ".opus")) {
                        g_string_append_printf(def_str, "<a class=\"xdxf-rref xdxf-snd\" href=\"sound://%s\">🔊</a>", l);
                    } else {
                        g_string_append_printf(def_str, "<img class=\"xdxf-rref xdxf-img\" src=\"%s\" />", l);
                    }
                    xmlFree(lctn);
                } else {
                    in_rref = TRUE;
                }
                has_inline_content = TRUE; pending_space = TRUE;
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"co")) {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                g_string_append(def_str, "<span class=\"xdxf-co\">");
                has_inline_content = TRUE;
            } else {
                xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                g_string_append_printf(def_str, "<span class=\"xdxf-%s\">", (const char*)inner_name);
                has_inline_content = TRUE;
            }
        } else if (inner_type == XML_READER_TYPE_END_ELEMENT) {
            if (xmlStrEqual(inner_name, (const xmlChar*)"k") || xmlStrEqual(inner_name, (const xmlChar*)"ar") || xmlStrEqual(inner_name, (const xmlChar*)"xdxf")) {
                // k handled natively by inner sub-loop or skip, ar is wrapper
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"b") || xmlStrEqual(inner_name, (const xmlChar*)"i") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"u") || xmlStrEqual(inner_name, (const xmlChar*)"sub") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"sup") || xmlStrEqual(inner_name, (const xmlChar*)"ul") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"ol") || xmlStrEqual(inner_name, (const xmlChar*)"li") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"p") || xmlStrEqual(inner_name, (const xmlChar*)"div") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"span") || xmlStrEqual(inner_name, (const xmlChar*)"br") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"hr") || xmlStrEqual(inner_name, (const xmlChar*)"blockquote")) {
                g_string_append_printf(def_str, "</%s>", (const char*)inner_name);
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref") || xmlStrEqual(inner_name, (const xmlChar*)"a") ||
                       xmlStrEqual(inner_name, (const xmlChar*)"iref") || xmlStrEqual(inner_name, (const xmlChar*)"rref")) {
                if (xmlStrEqual(inner_name, (const xmlChar*)"rref")) {
                    in_rref = FALSE;
                } else {
                    g_string_append(def_str, "</a>");
                }
            } else if (xmlStrEqual(inner_name, (const xmlChar*)"def")) {
                pending_space = FALSE; has_inline_content = FALSE;
                g_string_append(def_str, "</div>");
                if (def_nesting > 0) def_nesting--;
            } else {
                g_string_append(def_str, "</span>");
            }
        } else if (inner_type == XML_READER_TYPE_TEXT || inner_type == XML_READER_TYPE_CDATA) {
            const xmlChar *value = xmlTextReaderConstValue(reader);
            if (value) {
                if (in_rref) {
                    char *stripped = g_strstrip(g_strdup((const char*)value));
                    if (g_str_has_suffix(stripped, ".ogg") || g_str_has_suffix(stripped, ".wav") || g_str_has_suffix(stripped, ".mp3") || g_str_has_suffix(stripped, ".opus")) {
                        g_string_append_printf(def_str, "<a class=\"xdxf-rref xdxf-snd\" href=\"sound://%s\">🔊</a>", stripped);
                    } else {
                        g_string_append_printf(def_str, "<img class=\"xdxf-rref xdxf-img\" src=\"%s\" />", stripped);
                    }
                    g_free(stripped);
                    has_inline_content = TRUE;
                } else {
                    xdxf_flush_pending_space(def_str, &pending_space, has_inline_content, apply_special_formatting);
                    xdxf_append_escaped_text(def_str, (const char*)value, FALSE, apply_special_formatting);
                    has_inline_content = TRUE;
                }
            }
        } else if (inner_type == XML_READER_TYPE_WHITESPACE || inner_type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE) {
            if (apply_special_formatting) {
                const xmlChar *value = xmlTextReaderConstValue(reader);
                if (value) {
                    char *escaped = g_markup_escape_text((const char*)value, -1);
                    g_string_append(def_str, escaped);
                    g_free(escaped);
                }
            } else {
                pending_space = TRUE;
            }
            continue;
        }
    }
    g_string_append(def_str, "</div>");
    xmlFreeTextReader(reader);
    g_free(raw);

    if (out_len) *out_len = def_str->len;
    if (out_to_free) *out_to_free = def_str->str;
    else g_string_free(def_str, TRUE);

    return out_to_free ? *out_to_free : NULL;
}

static void extract_keys_and_add(const char *ar_start, size_t d_len, size_t d_off, DictHwBuilder *hw) {
    const char *p = ar_start;
    const char *end = ar_start + d_len;
    while (p < end) {
        const char *k_start = strstr(p, "<k>");
        if (!k_start || k_start >= end) break;
        k_start += 3;
        const char *k_end = strstr(k_start, "</k>");
        if (!k_end || k_end >= end) break;
        
        size_t k_len = k_end - k_start;
        if (k_len > 0) {
            char *k_raw = g_strndup(k_start, k_len);
            char *k_norm = xdxf_collapse_whitespace(k_raw, TRUE);
            if (k_norm && *k_norm) {
                dict_hw_builder_add(hw, k_norm, strlen(k_norm), d_off, d_len);
            }
            g_free(k_norm);
            g_free(k_raw);
        }
        p = k_end + 4;
    }
}

static void parse_xdxf_metadata_from_file(const char *xml_path, char **name, char **slang, char **tlang, int *lousy_format) {
    xmlTextReaderPtr reader = xmlReaderForFile(xml_path, NULL, XML_PARSE_HUGE);
    if (!reader) return;
    while (xmlTextReaderRead(reader) == 1) {
        if (xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT) {
            const xmlChar *n = xmlTextReaderConstLocalName(reader);
            if (xmlStrEqual(n, (const xmlChar*)"xdxf")) {
                xmlChar *f = xmlTextReaderGetAttribute(reader, (const xmlChar*)"format");
                if (f) {
                    if (g_ascii_strcasecmp((const char*)f, "visual") == 0) *lousy_format = XDXF_LOUSY_FORMAT_VISUAL;
                    else if (g_ascii_strcasecmp((const char*)f, "logical") == 0) *lousy_format = XDXF_LOUSY_FORMAT_LOGICAL;
                    xmlFree(f);
                }
                xmlChar *lf = xmlTextReaderGetAttribute(reader, (const xmlChar*)"lang_from");
                if (lf) { *slang = g_strdup((const char*)lf); xmlFree(lf); }
                xmlChar *lt = xmlTextReaderGetAttribute(reader, (const xmlChar*)"lang_to");
                if (lt) { *tlang = g_strdup((const char*)lt); xmlFree(lt); }
                xmlChar *fn = xmlTextReaderGetAttribute(reader, (const xmlChar*)"full_name");
                if (fn) { *name = g_strdup((const char*)fn); xmlFree(fn); }
            } else if (xmlStrEqual(n, (const xmlChar*)"full_title") || xmlStrEqual(n, (const xmlChar*)"full_name")) {
                xmlChar *val = xmlTextReaderReadString(reader);
                if (val) {
                    if (*name) g_free(*name);
                    *name = g_strdup((const char*)val);
                    xmlFree(val);
                }
            } else if (xmlStrEqual(n, (const xmlChar*)"title") || xmlStrEqual(n, (const xmlChar*)"description")) {
                xmlChar *val = xmlTextReaderReadString(reader);
                if (val && !*name) {
                    *name = g_strdup((const char*)val);
                    xmlFree(val);
                }
            } else if (xmlStrEqual(n, (const xmlChar*)"ar")) {
                break; // Stop parsing metadata once we hit articles
            }
        }
    }
    xmlFreeTextReader(reader);
}

static void build_xdxf_index_only_cache(const char *xml_path, const char *orig_path, const char *hw_path, volatile gint *cancel_flag, gint expected) {
    int fd = open(xml_path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) { close(fd); return; }
    size_t size = st.st_size;
    const char *mmap_data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mmap_data == MAP_FAILED) return;

    DictHwBuilder *hw = dict_hw_builder_new(hw_path);
    if (!hw) { munmap((void*)mmap_data, size); return; }

    const char *p = mmap_data;
    const char *end = mmap_data + size;
    const char *ar_start = NULL;
    int total_ars = 0;

    while (p < end) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
            dict_hw_builder_free(hw);
            munmap((void*)mmap_data, size);
            unlink(hw_path);
            return;
        }

        if (!ar_start) {
            const char *match = memmem(p, end - p, "<ar", 3);
            if (!match) break;
            if (match + 3 < end && (match[3] == '>' || g_ascii_isspace(match[3]))) {
                ar_start = match;
                p = match + 3;
            } else {
                p = match + 1;
            }
        } else {
            const char *ar_end = memmem(p, end - p, "</ar>", 5);
            if (!ar_end) break;
            ar_end += 5;
            size_t d_off = ar_start - mmap_data;
            size_t d_len = ar_end - ar_start;
            
            extract_keys_and_add(ar_start, d_len, d_off, hw);
            
            p = ar_end;
            ar_start = NULL;
            total_ars++;
            if (total_ars % 1000 == 0) settings_scan_progress_notify(orig_path, 30 + (total_ars / 5000) % 65);
        }
    }

    char *name = NULL, *slang = NULL, *tlang = NULL;
    int lousy_format = XDXF_LOUSY_FORMAT_UNKNOWN;
    int standard = xdxf_detect_standard_from_file(xml_path);
    parse_xdxf_metadata_from_file(xml_path, &name, &slang, &tlang, &lousy_format);

    dict_hw_builder_set_metadata(hw, "source_path", orig_path);
    dict_hw_builder_set_metadata(hw, "xdxf_standard", standard == XDXF_STANDARD_LOUSY ? "1" : "0");
    dict_hw_builder_set_metadata(hw, "xdxf_lousy_format", g_strdup_printf("%d", lousy_format));
    if (name) dict_hw_builder_set_metadata(hw, "name", name);
    if (slang) dict_hw_builder_set_metadata(hw, "source_lang", slang);
    if (tlang) dict_hw_builder_set_metadata(hw, "target_lang", tlang);

    g_free(name); g_free(slang); g_free(tlang);

    dict_hw_builder_finalize(hw);
    munmap((void*)mmap_data, size);

    struct stat hw_src_st;
    if (stat(orig_path, &hw_src_st) == 0) {
        struct utimbuf times = { .actime = hw_src_st.st_mtime, .modtime = hw_src_st.st_mtime };
        utime(hw_path, &times);
    }
}

DictMmap* parse_xdxf_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    dict_cache_ensure_dir();

    char *hw_path = dict_hw_index_path_for(path);
    gboolean needs_build = !dict_cache_is_valid(hw_path, path);

    char *res_dir = g_strdup_printf("%s.res", hw_path);
    char *xml_path = NULL;

    if (needs_build) {
        if (ends_with_ci(path, ".tar.bz2") || ends_with_ci(path, ".tar.gz") || ends_with_ci(path, ".tar.xz") || ends_with_ci(path, ".tgz") || ends_with_ci(path, ".zip")) {
            xml_path = extract_xdxf_xml_from_archive(path, res_dir, cancel_flag, expected);
        } else if (ends_with_ci(path, ".xdxf.dz")) {
            g_mkdir_with_parents(res_dir, 0755);
            xml_path = decompress_xdxf_dz(path, res_dir, cancel_flag, expected);
        } else {
            xml_path = g_strdup(path);
        }

        if (!xml_path) {
            g_free(res_dir); g_free(hw_path); return NULL;
        }

        if (ends_with_ci(xml_path, ".xdxf.dz")) {
            char *temp_dir = g_path_get_dirname(xml_path);
            char *uncompressed_xml = decompress_xdxf_dz(xml_path, res_dir, cancel_flag, expected);
            if (xml_path && g_strcmp0(xml_path, path) != 0) g_unlink(xml_path);
            g_free(xml_path);
            xml_path = uncompressed_xml;
            g_free(temp_dir);
        }

        if (!xml_path) {
            g_free(res_dir); g_free(hw_path); return NULL;
        }

        build_xdxf_index_only_cache(xml_path, path, hw_path, cancel_flag, expected);

        if (xml_path && g_strcmp0(xml_path, path) != 0) {
            // we extracted a temp xml, if it was just uncompressed locally, delete it, or keep it in res_dir
            if (!ends_with_ci(path, ".tar.bz2") && !ends_with_ci(path, ".tar.gz") && !ends_with_ci(path, ".tar.xz") && !ends_with_ci(path, ".zip") && !ends_with_ci(path, ".tgz")) {
                unlink(xml_path); 
            }
            g_free(xml_path);
        } else if (xml_path) {
            g_free(xml_path);
        }
    }

    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
        g_free(hw_path); g_free(res_dir); return NULL;
    }

    FlatIndex *idx = flat_index_open(hw_path);
    if (!idx) { g_free(hw_path); g_free(res_dir); return NULL; }

    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = -1;
    dict->is_xdxf = TRUE;
    dict->index = idx;
    
    const char *xdxf_std = flat_index_get_metadata(idx, "xdxf_standard");
    if (xdxf_std && strcmp(xdxf_std, "1") == 0) dict->xdxf_standard = XDXF_STANDARD_LOUSY;
    else dict->xdxf_standard = XDXF_STANDARD_STRICT;

    const char *xdxf_lousy = flat_index_get_metadata(idx, "xdxf_lousy_format");
    if (xdxf_lousy) dict->xdxf_lousy_format = atoi(xdxf_lousy);

    const char *name = flat_index_get_metadata(idx, "name");
    if (name) dict->name = g_strdup(name);
    
    const char *slang = flat_index_get_metadata(idx, "source_lang");
    if (slang) dict->source_lang = g_strdup(slang);
    
    const char *tlang = flat_index_get_metadata(idx, "target_lang");
    if (tlang) dict->target_lang = g_strdup(tlang);

    if (ends_with_ci(path, ".tar.bz2") || ends_with_ci(path, ".tar.gz") || ends_with_ci(path, ".tar.xz") || ends_with_ci(path, ".tgz") || ends_with_ci(path, ".zip")) {
        dict->resource_reader = resource_reader_open_archive(path, res_dir);
        dict->resource_dir = g_strdup(res_dir);
    } else if (g_file_test(res_dir, G_FILE_TEST_IS_DIR)) {
        dict->resource_dir = g_strdup(res_dir);
    }
    dict->source_dir = g_canonicalize_filename(g_path_get_dirname(path), NULL);

    g_free(hw_path);
    g_free(res_dir);
    settings_scan_progress_notify(path, 100);
    return dict;
}
