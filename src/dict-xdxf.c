#include "dict-mmap.h"
#include "flat-index.h"
#include "dict-chunked.h"
#include "dict-cache.h"
#include "dict-cache-builder.h"
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
    DictCacheBuilder *builder;
    GArray *entries;
    char *dict_name;
    char *source_lang;
    char *target_lang;
    char *resource_dir;
    int xdxf_standard;
    int default_lousy_format;
} XdxfParserState;

enum {
    XDXF_STANDARD_STRICT = 0,
    XDXF_STANDARD_LOUSY = 1
};

enum {
    XDXF_LOUSY_FORMAT_UNKNOWN = 0,
    XDXF_LOUSY_FORMAT_LOGICAL = 1,
    XDXF_LOUSY_FORMAT_VISUAL = 2
};

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

static gboolean buffer_contains_literal(const char *buf, size_t len, const char *needle) {
    if (!buf || !needle) return FALSE;
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || len < needle_len) return FALSE;
    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return TRUE;
        }
    }
    return FALSE;
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
        char ch = g_ascii_toupper(*p);
        if (!(ch == 'I' || ch == 'V' || ch == 'X' || ch == 'L' || ch == 'C' || ch == 'D' || ch == 'M')) {
            break;
        }
        p++;
    }
    if (p == start) return FALSE;
    while (*p && g_ascii_isspace(*p)) p++;
    return (*p == '\0');
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

static void xdxf_append_collapsed_escaped_text(GString *out, const char *text, gboolean trim_edges) {
    if (!out || !text || !*text) return;

    char *collapsed = xdxf_collapse_whitespace(text, trim_edges);
    if (!collapsed || !*collapsed) {
        g_free(collapsed);
        return;
    }

    if (out->len == 0) {
        g_strchug(collapsed);
    } else if (g_ascii_isspace(out->str[out->len - 1])) {
        g_strchug(collapsed);
    }

    if (!*collapsed) {
        g_free(collapsed);
        return;
    }

    char *escaped = g_markup_escape_text(collapsed, -1);
    g_string_append(out, escaped);
    g_free(escaped);
    g_free(collapsed);
}

static void xdxf_append_space_if_needed(GString *out) {
    if (!out || out->len == 0) {
        return;
    }

    char last = out->str[out->len - 1];
    if (g_ascii_isspace(last)) {
        return;
    }

    g_string_append_c(out, ' ');
}

static void xdxf_flush_pending_space(GString *out, gboolean *pending_space, gboolean has_inline_content) {
    if (!pending_space || !*pending_space) {
        return;
    }

    if (has_inline_content) {
        xdxf_append_space_if_needed(out);
    }

    *pending_space = FALSE;
}

static void process_xml_xdxf(xmlTextReaderPtr reader, XdxfParserState *state, const char *path, volatile gint *cancel_flag, gint expected) {
    int ret = xmlTextReaderRead(reader);
    int total_ars = 0;
    while (ret == 1) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;

        const xmlChar *name = xmlTextReaderConstLocalName(reader);
        int type = xmlTextReaderNodeType(reader);
        if (!name) {
            ret = xmlTextReaderRead(reader);
            continue;
        }
        
        if (type == XML_READER_TYPE_ELEMENT && xmlStrEqual(name, (const xmlChar*)"ar")) {
            total_ars++;
            if (total_ars % 100 == 0) {
                /* We don't know total ARs, so we use a heuristic or just incrementing progress if possible.
                 * Actually, let's use byte offset from the reader if available. */
                /* int64_t offset = xmlTextReaderByteConsumed(reader); */
                /* We don't have total size here easily, but we can assume some progress. */
                settings_scan_progress_notify(path, 30 + (total_ars / 500) % 65);
            }
        }

        if (type == XML_READER_TYPE_ELEMENT) {
            if (xmlStrEqual(name, (const xmlChar*)"xdxf")) {
                xmlChar *root_format = xmlTextReaderGetAttribute(reader, (const xmlChar*)"format");
                if (state->xdxf_standard == XDXF_STANDARD_LOUSY && root_format) {
                    if (g_ascii_strcasecmp((const char*)root_format, "visual") == 0) {
                        state->default_lousy_format = XDXF_LOUSY_FORMAT_VISUAL;
                    } else if (g_ascii_strcasecmp((const char*)root_format, "logical") == 0) {
                        state->default_lousy_format = XDXF_LOUSY_FORMAT_LOGICAL;
                    }
                }
                if (root_format) {
                    xmlFree(root_format);
                }

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
            } else if (xmlStrEqual(name, (const xmlChar*)"full_title") ||
                       xmlStrEqual(name, (const xmlChar*)"full_name")) {
                xmlChar *val = xmlTextReaderReadString(reader);
                if (val) {
                    if (state->dict_name) g_free(state->dict_name);
                    state->dict_name = g_strdup((const char*)val);
                    xmlFree(val);
                }
            } else if (xmlStrEqual(name, (const xmlChar*)"title") ||
                       xmlStrEqual(name, (const xmlChar*)"description")) {
                xmlChar *val = xmlTextReaderReadString(reader);
                if (val) {
                    if (!state->dict_name) {
                        state->dict_name = g_strdup((const char*)val);
                    }
                    xmlFree(val);
                }
            }
 else if (xmlStrEqual(name, (const xmlChar*)"ar")) {
                int ar_depth = xmlTextReaderDepth(reader);
                GString *hw_str  = g_string_new("");
                int ar_lousy_format = state->default_lousy_format;
                xmlChar *ar_format = xmlTextReaderGetAttribute(reader, (const xmlChar*)"f");
                if (state->xdxf_standard == XDXF_STANDARD_LOUSY && ar_format) {
                    if (((const char*)ar_format)[0] == 'v' || ((const char*)ar_format)[0] == 'V') {
                        ar_lousy_format = XDXF_LOUSY_FORMAT_VISUAL;
                    } else if (((const char*)ar_format)[0] == 'l' || ((const char*)ar_format)[0] == 'L') {
                        ar_lousy_format = XDXF_LOUSY_FORMAT_LOGICAL;
                    }
                }
                if (ar_format) {
                    xmlFree(ar_format);
                }

                // Wrap the article with profile/format classes for CSS routing.
                GString *def_str = g_string_new("");
                g_string_append_printf(def_str, "<div class=\"dictionary-entry xdxf-ar %s %s\">",
                                       xdxf_profile_class(state->xdxf_standard),
                                       xdxf_lousy_format_class(ar_lousy_format));
                int def_nesting = 0;
                gboolean pending_space = FALSE;
                gboolean has_inline_content = FALSE;
                gboolean seen_roman_section = FALSE;

                while (xmlTextReaderRead(reader) == 1 && xmlTextReaderDepth(reader) > ar_depth) {
                    const xmlChar *inner_name = xmlTextReaderConstLocalName(reader);
                    int inner_type = xmlTextReaderNodeType(reader);

                    if (inner_type == XML_READER_TYPE_ELEMENT) {
                        if (xmlStrEqual(inner_name, (const xmlChar*)"def")) {
                            pending_space = FALSE;
                            has_inline_content = FALSE;
                            def_nesting++;
                            g_string_append_printf(def_str, "<div class=\"xdxf-def xdxf-def-lvl-%d\">", def_nesting);
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"k")) {
                            int k_depth = xmlTextReaderDepth(reader);
                            xmlNodePtr node = xmlTextReaderExpand(reader);
                            if (node) {
                                xmlChar *val = xmlNodeGetContent(node);
                                if (val) {
                                    char *normalized = xdxf_collapse_whitespace((const char*)val, TRUE);
                                    if (normalized && *normalized) {
                                        if (hw_str->len > 0) g_string_append(hw_str, "; ");
                                        g_string_append(hw_str, normalized);
                                    }
                                    g_free(normalized);
                                    xmlFree(val);
                                }
                            }
                            while (xmlTextReaderRead(reader) == 1 && xmlTextReaderDepth(reader) > k_depth) {
                            }
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"a")) {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            xmlChar *href_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"href");
                            if (href_attr) {
                                g_string_append_printf(def_str, "<a class=\"xdxf-a\" href=\"%s\">", (const char*)href_attr);
                                xmlFree(href_attr);
                            } else {
                                g_string_append(def_str, "<a class=\"xdxf-a\">");
                            }
                            has_inline_content = TRUE;
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
                                   xmlStrEqual(inner_name, (const xmlChar*)"br") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"hr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"blockquote")) {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            // Preserve native formatting tags but attach the class
                            g_string_append_printf(def_str, "<%s class=\"xdxf-%s\">", (const char*)inner_name, (const char*)inner_name);
                            has_inline_content = TRUE;
                            
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"c")) {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            // Map explicit color definitions
                            xmlChar *c_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"c");
                            if (c_attr) {
                                const char *c_attr_str = (const char*)c_attr;
                                gboolean apply_special_formatting = (
                                    state->xdxf_standard == XDXF_STANDARD_LOUSY &&
                                    ar_lousy_format == XDXF_LOUSY_FORMAT_VISUAL
                                );
                                if (apply_special_formatting) {
                                    gboolean is_blue = (g_ascii_strcasecmp(c_attr_str, "blue") == 0);
                                    gboolean is_numeric_marker = FALSE;
                                    if (is_blue) {
                                        xmlNodePtr c_node = xmlTextReaderExpand(reader);
                                        if (c_node) {
                                            xmlChar *c_val = xmlNodeGetContent(c_node);
                                            if (c_val) {
                                                is_numeric_marker = xdxf_is_number_marker_text((const char*)c_val);
                                                xmlFree(c_val);
                                            }
                                        }
                                    }
                                    gboolean is_roman_marker = FALSE;
                                    if (is_blue && !is_numeric_marker) {
                                        xmlNodePtr c_node = xmlTextReaderExpand(reader);
                                        if (c_node) {
                                            xmlChar *c_val = xmlNodeGetContent(c_node);
                                            if (c_val) {
                                                is_roman_marker = xdxf_is_roman_marker_text((const char*)c_val);
                                                xmlFree(c_val);
                                            }
                                        }
                                    }

                                    if (is_numeric_marker) {
                                        g_string_append_printf(def_str, "<span class=\"xdxf-c xdxf-c-blue-num\" style=\"color: %s;\">", c_attr_str);
                                    } else if (is_roman_marker) {
                                        if (seen_roman_section) {
                                            g_string_append_printf(def_str, "<span class=\"xdxf-c xdxf-c-blue-roman xdxf-c-blue-roman-break\" style=\"color: %s;\">", c_attr_str);
                                        } else {
                                            g_string_append_printf(def_str, "<span class=\"xdxf-c xdxf-c-blue-roman\" style=\"color: %s;\">", c_attr_str);
                                        }
                                        seen_roman_section = TRUE;
                                    } else {
                                        g_string_append_printf(def_str, "<span class=\"xdxf-c\" style=\"color: %s;\">", c_attr_str);
                                    }
                                } else {
                                    // Strict XDXF and lousy logical: simple color span (matches 1356a58)
                                    g_string_append_printf(def_str, "<span class=\"xdxf-c\" style=\"color: %s;\">", c_attr_str);
                                }
                                xmlFree(c_attr);
                            } else {
                                g_string_append(def_str, "<span class=\"xdxf-c\">");
                            }
                            has_inline_content = TRUE;
                            
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref")) {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            // Map dictionary cross-reference links
                            xmlChar *k_attr = xmlTextReaderGetAttribute(reader, (const xmlChar*)"k");
                            if (k_attr) {
                                char *uri_attr = g_uri_escape_string((const char*)k_attr, NULL, TRUE);
                                g_string_append_printf(def_str, "<a href=\"dict://%s\" class=\"xdxf-kref\">", uri_attr);
                                g_free(uri_attr);
                                xmlFree(k_attr);
                            } else {
                                // If 'k' is missing, use the text content as the link target
                                if (xmlTextReaderExpand(reader) != NULL) {
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
                            has_inline_content = TRUE;
                            
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"iref")) {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            xmlChar *href = xmlTextReaderGetAttribute(reader, (const xmlChar*)"href");
                            if (href) {
                                g_string_append_printf(def_str, "<a class=\"xdxf-iref\" href=\"%s\">", (const char*)href);
                                xmlFree(href);
                            } else {
                                g_string_append(def_str, "<a class=\"xdxf-iref\">");
                            }
                            has_inline_content = TRUE;
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"rref")) {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            if (has_inline_content) {
                                xdxf_append_space_if_needed(def_str);
                            }
                            xmlChar *lctn = xmlTextReaderGetAttribute(reader, (const xmlChar*)"lctn");
                            if (lctn) {
                                const char *l = (const char*)lctn;
                                if (g_str_has_suffix(l, ".ogg") || g_str_has_suffix(l, ".wav") || g_str_has_suffix(l, ".mp3") || g_str_has_suffix(l, ".opus")) {
                                    g_string_append_printf(def_str, "<a class=\"xdxf-rref xdxf-snd\" href=\"sound://%s\">🔊</a>", l);
                                } else {
                                    g_string_append_printf(def_str, "<img class=\"xdxf-rref xdxf-img\" src=\"%s\" />", l);
                                }
                                xmlFree(lctn);
                            }
                            has_inline_content = TRUE;
                            pending_space = TRUE;
                        } else {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            // Everything else (dtrn, ex, co, abr, tr) maps dynamically to semantic span elements
                            g_string_append_printf(def_str, "<span class=\"xdxf-%s\">", (const char*)inner_name);
                            has_inline_content = TRUE;
                        }
                    } else if (inner_type == XML_READER_TYPE_END_ELEMENT) {
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
                                   xmlStrEqual(inner_name, (const xmlChar*)"br") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"hr") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"blockquote")) {
                            g_string_append_printf(def_str, "</%s>", (const char*)inner_name);
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"kref") || 
                                   xmlStrEqual(inner_name, (const xmlChar*)"a") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"iref") ||
                                   xmlStrEqual(inner_name, (const xmlChar*)"rref")) {
                            g_string_append(def_str, "</a>");
                        } else if (xmlStrEqual(inner_name, (const xmlChar*)"def")) {
                            pending_space = FALSE;
                            has_inline_content = FALSE;
                            g_string_append(def_str, "</div>");
                            if (def_nesting > 0) def_nesting--;
                        } else {
                            g_string_append(def_str, "</span>");
                        }
                    } else if (inner_type == XML_READER_TYPE_TEXT ||
                               inner_type == XML_READER_TYPE_CDATA) {
                        const xmlChar *value = xmlTextReaderConstValue(reader);
                        if (value) {
                            xdxf_flush_pending_space(def_str, &pending_space, has_inline_content);
                            xdxf_append_collapsed_escaped_text(def_str, (const char*)value, FALSE);
                            has_inline_content = TRUE;
                        }
                    } else if (inner_type == XML_READER_TYPE_WHITESPACE ||
                               inner_type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE) {
                        pending_space = TRUE;
                        continue;
                    }
                }
                
                g_string_append(def_str, "</div>");
                
                // Write payload to index
                if (hw_str->len > 0) {
                    TreeEntry entry;
                    uint64_t hw_off = 0, def_off = 0;
                    
                    dict_cache_builder_add_headword(state->builder, hw_str->str, hw_str->len, &hw_off);
                    dict_cache_builder_add_definition(state->builder, def_str->str, def_str->len, &def_off);
                    
                    entry.h_off = (uint32_t)hw_off;
                    entry.h_len = (uint32_t)hw_str->len;
                    entry.d_off = (uint32_t)def_off;
                    entry.d_len = (uint32_t)def_str->len;
                    
                    g_array_append_val(state->entries, entry);
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
                if (!buffer_contains_literal(data, size, "xdxf-profile-")) {
                    /* Cached payload predates XDXF profile classes; rebuild cache. */
                    munmap((void*)data, size);
                    close(fd);
                    unlink(cache_path);
                    fd = -1;
                    goto rebuild_xdxf_cache;
                }

                DictMmap *dict = g_new0(DictMmap, 1);
                dict->fd = fd;
                close(dict->fd);
                dict->fd = -1;
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
                if (dict_cache_is_compressed(dict->data, dict->size)) {
                    dict->is_compressed = TRUE;
                    dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, (const DictCacheHeader*)dict->data);
                    if (!dict->chunk_reader || !flat_index_validate(dict->index)) {
                        fprintf(stderr, "[XDXF] Compressed cache validation failed for %s; rebuilding.\n", path);
                        flat_index_close(dict->index);
                        if (dict->chunk_reader) dict_chunk_reader_free(dict->chunk_reader);
                        resource_reader_close(dict->resource_reader);
                        g_free(dict->name);
                        g_free(dict->source_lang);
                        g_free(dict->target_lang);
                        g_free(dict->resource_dir);
                        g_free(dict->source_dir);
                        munmap((void*)dict->data, dict->size);
                        close(dict->fd);
                        fd = -1;
                        g_free(dict);
                        unlink(cache_path);
                    } else {
                        g_free(cache_path);
                        return dict;
                    }
                } else {
                    g_free(cache_path);
                    return dict;
                }
            }
            if (fd >= 0) close(fd);
        }
    }

rebuild_xdxf_cache:
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

    DictCacheBuilder *builder = dict_cache_builder_new(cache_path, 10000); // hint 10k entries
    if (!builder) {
        if (xml_path != path) { unlink(xml_path); g_free(xml_path); }
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }

    XdxfParserState state = {0};
    state.builder = builder;
    state.entries = g_array_new(FALSE, TRUE, sizeof(TreeEntry));
    state.resource_dir = res_dir;
    state.xdxf_standard = xdxf_detect_standard_from_file(xml_path);
    state.default_lousy_format = XDXF_LOUSY_FORMAT_UNKNOWN;

    process_xml_xdxf(reader, &state, path, cancel_flag, expected);

    xmlFreeTextReader(reader);
    if (xml_path != path) {
        /* Keep in res_dir */
    }

    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
        dict_cache_builder_free(builder);
        g_array_free(state.entries, TRUE);
        g_free(state.dict_name);
        g_free(state.source_lang);
        g_free(state.target_lang);
        if (xml_path != path) { unlink(xml_path); g_free(xml_path); }
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }

    if (state.entries->len > 0) {
        /* Sort entries for binary search */
        dict_cache_builder_flush(builder);
        
        int sort_fd = open(cache_path, O_RDONLY);
        if (sort_fd >= 0) {
            struct stat st_tmp;
            fstat(sort_fd, &st_tmp);
            void *sort_mmap = mmap(NULL, (size_t)st_tmp.st_size, PROT_READ, MAP_PRIVATE, sort_fd, 0);
            if (sort_mmap != MAP_FAILED) {
                flat_index_sort_entries((TreeEntry*)state.entries->data, state.entries->len, sort_mmap, (size_t)st_tmp.st_size);
                munmap(sort_mmap, (size_t)st_tmp.st_size);
            }
            close(sort_fd);
        }
        
        dict_cache_builder_finalize(builder, (TreeEntry*)state.entries->data, state.entries->len);
    } else {
        fprintf(stderr, "[XDXF] No entries parsed from %s\n", xml_path);
        dict_cache_builder_free(builder);
        g_array_free(state.entries, TRUE);
        g_free(state.dict_name);
        g_free(state.source_lang);
        g_free(state.target_lang);
        if (xml_path != path) { unlink(xml_path); g_free(xml_path); }
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }
    dict_cache_builder_free(builder);

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
    if (fd < 0) {
        g_array_free(state.entries, TRUE);
        g_free(state.dict_name);
        g_free(state.source_lang);
        g_free(state.target_lang);
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(DictCacheHeader)) {
        close(fd);
        g_array_free(state.entries, TRUE);
        g_free(state.dict_name);
        g_free(state.source_lang);
        g_free(state.target_lang);
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }
    size_t size = st.st_size;
    const char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        g_array_free(state.entries, TRUE);
        g_free(state.dict_name);
        g_free(state.source_lang);
        g_free(state.target_lang);
        g_free(res_dir);
        g_free(cache_path);
        return NULL;
    }
    
    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = fd;
    close(dict->fd);
    dict->fd = -1;
    dict->data = data;
    dict->size = size;
    dict->name = state.dict_name;
    dict->source_lang = state.source_lang;
    dict->target_lang = state.target_lang;
    dict->index = flat_index_open(data, size);

    if (dict_cache_is_compressed(dict->data, dict->size)) {
        dict->is_compressed = TRUE;
        dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, (const DictCacheHeader*)dict->data);
    }
    
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
