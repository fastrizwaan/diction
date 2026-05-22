#include "dict-dsl-index.h"
#include "dict-dsl-scanner.h"
#include "dict-cache-builder.h"
#include "flat-index.h"
#include "dict-cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *headword;
    uint32_t d_off;
    uint32_t d_len;
} DslIndexEntry;

gboolean build_dsl_index_only_cache(const char *dsl_path)
{

    DslScanner *s = dsl_scanner_open(dsl_path);
    if (!s) return FALSE;

    uint32_t source_encoding = 0;
    if (s->is_utf16le) source_encoding = 1;
    else if (s->is_utf16be) source_encoding = 2;

    /* Build SQLite headword index */
    char *hw_path = dict_hw_index_path_for(dsl_path);
    DictHwBuilder *hw = dict_hw_builder_new(hw_path);
    if (!hw) {
        g_free(hw_path);
        dsl_scanner_close(s);
        return FALSE;
    }

    char enc_str[4];
    snprintf(enc_str, sizeof(enc_str), "%u", source_encoding);
    dict_hw_builder_set_metadata(hw, "source_encoding", enc_str);
    dict_hw_builder_set_metadata(hw, "source_path", dsl_path);

    /* Store header metadata in SQLite if available */
    {
        char *name = NULL, *src_lang = NULL, *tgt_lang = NULL;
        dsl_parse_header_internal(dsl_path, &name, &src_lang, &tgt_lang);
        if (name) {
            dict_hw_builder_set_metadata(hw, "dict_name", name);
            g_free(name);
        }
        if (src_lang) {
            dict_hw_builder_set_metadata(hw, "source_lang", src_lang);
            g_free(src_lang);
        }
        if (tgt_lang) {
            dict_hw_builder_set_metadata(hw, "target_lang", tgt_lang);
            g_free(tgt_lang);
        }
    }

    size_t entry_cap = 100000;
    size_t entry_count = 0;
    DslIndexEntry *entries = malloc(entry_cap * sizeof(DslIndexEntry));

    char **hws = NULL;
    size_t hw_count = 0;
    size_t hw_cap = 0;

    size_t line_cap = 8388608;
    char *line_buf = malloc(line_cap);
    size_t line_len;
    uint64_t uncomp_offset;
    size_t uncomp_len;

    uint64_t def_offset = 0;
    uint64_t def_len = 0;
    int in_def = 0;

    #define FLUSH_HEADWORDS_HW() do { \
        if (in_def && hw_count > 0 && def_len > 0) { \
            for (size_t _i = 0; _i < hw_count; _i++) { \
                if (entry_count >= entry_cap) { \
                    entry_cap *= 2; \
                    entries = realloc(entries, entry_cap * sizeof(DslIndexEntry)); \
                } \
                entries[entry_count].headword = hws[_i]; \
                entries[entry_count].d_off = (uint32_t)def_offset; \
                entries[entry_count].d_len = (uint32_t)def_len; \
                entry_count++; \
            } \
        } else { \
            for (size_t _i = 0; _i < hw_count; _i++) free(hws[_i]); \
        } \
        hw_count = 0; \
        in_def = 0; \
        def_len = 0; \
    } while (0)

    while (1) {
        int read_res = dsl_scanner_read_line(s, line_buf, line_cap, &line_len, &uncomp_offset, &uncomp_len);
        if (read_res == 0) break;
        if (line_len == 0) continue;

        if (line_buf[0] == '#') continue;

        int is_indented = (line_buf[0] == ' ' || line_buf[0] == '\t');
        if (!is_indented) {
            if (in_def) FLUSH_HEADWORDS_HW();

            if (line_buf[0] == '{' && line_len > 1 && line_buf[1] == '{') continue;

            const char *lptr = line_buf;
            size_t sub_start = 0;
            for (size_t i = 0; i <= line_len; i++) {
                if (i == line_len || lptr[i] == ';') {
                    if (lptr[i] == ';' && i > 0 && lptr[i-1] == '\\') {
                        int bs_count = 0;
                        for (int k = (int)i - 1; k >= 0 && lptr[k] == '\\'; k--) bs_count++;
                        if (bs_count % 2 != 0) continue;
                    }

                    int is_entity = 0;
                    if (i > 0 && lptr[i] == ';') {
                        for (int j = (int)i - 1; j >= (int)sub_start && j >= (int)i - 10; j--) {
                            if (lptr[j] == '&') { is_entity = 1; break; }
                            if (!g_ascii_isalnum(lptr[j]) && lptr[j] != '#') break;
                        }
                    }
                    if (is_entity && i < line_len) continue;

                    size_t sub_off = sub_start;
                    size_t sub_len = i - sub_start;

                    while (sub_len > 0 && g_ascii_isspace(lptr[sub_off])) { sub_off++; sub_len--; }
                    while (sub_len > 0 && g_ascii_isspace(lptr[sub_off + sub_len - 1])) { sub_len--; }

                    if (sub_len > 0) {
                        int has_alnum = 0;
                        for (size_t k = 0; k < sub_len; k++) {
                            if (g_ascii_isalnum(lptr[sub_off + k]) || (unsigned char)lptr[sub_off + k] >= 0x80) {
                                has_alnum = 1;
                                break;
                            }
                        }

                        if (hw_count > 0 && !has_alnum) {
                            char *old = hws[hw_count - 1];
                            hws[hw_count - 1] = g_strdup_printf("%s ; %.*s", old, (int)sub_len, lptr + sub_off);
                            free(old);
                        } else {
                            if (hw_count >= hw_cap) {
                                hw_cap = hw_cap == 0 ? 16 : hw_cap * 2;
                                hws = realloc(hws, hw_cap * sizeof(char*));
                            }
                            hws[hw_count++] = g_strndup(lptr + sub_off, sub_len);
                        }
                    }
                    sub_start = i + 1;
                }
            }

            def_offset = uncomp_offset + uncomp_len;
            def_len = 0;
        } else {
            in_def  = 1;
            def_len = (uncomp_offset + uncomp_len) - def_offset;
        }
    }

    FLUSH_HEADWORDS_HW();
    free(hws);
    free(line_buf);
    dsl_scanner_close(s);

    if (entry_count == 0) {
        free(entries);
        dict_hw_builder_free(hw);
        g_free(hw_path);
        return FALSE;
    }

    /* Insert into SQLite HW index (order does not matter — SQLite sorts) */
    for (size_t i = 0; i < entry_count; i++) {
        dict_hw_builder_add(hw, entries[i].headword, strlen(entries[i].headword),
                            entries[i].d_off, entries[i].d_len);
        g_free(entries[i].headword);
    }
    free(entries);

    gboolean ok = dict_hw_builder_finalize(hw);
    g_free(hw_path);
    return ok;
}
