#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

/* ── Multi-headword aware DSL parser ──────────────────────
 * DSL format allows N consecutive non-indented lines as headwords
 * followed by indented definition lines.  ALL headwords share the
 * same definition block.
 *
 *   a          ← headword 1
 *   ए          ← headword 2
 *   ē          ← headword 3
 *   	[b]...[/b]  ← definition (starts with space/tab)
 */

typedef struct {
    size_t offset;
    size_t length;
} HwSpan;

static void parse_dsl_into_tree(DictMmap *dict) {
    const char *p   = dict->data;
    const char *end = p + dict->size;

    /* Dynamic headword accumulator */
    HwSpan *hws     = NULL;
    size_t  hw_count = 0;
    size_t  hw_cap   = 0;

    size_t def_offset = 0;
    size_t def_len    = 0;
    int    in_def     = 0;
    int    word_count = 0;

    /* Flush: insert every collected headword with the current def block */
    #define FLUSH_HEADWORDS() do {                                       \
        if (in_def && hw_count > 0 && def_len > 0) {                    \
            for (size_t _i = 0; _i < hw_count; _i++) {                  \
                splay_tree_insert(dict->index,                           \
                    hws[_i].offset, hws[_i].length,                      \
                    def_offset, def_len);                                 \
                word_count++;                                            \
            }                                                            \
        }                                                                \
        hw_count = 0;                                                    \
        in_def   = 0;                                                    \
        def_len  = 0;                                                    \
    } while (0)

    while (p < end) {
        const char *line_start = p;

        /* Advance to end-of-line */
        while (p < end && *p != '\n') p++;
        size_t len = (size_t)(p - line_start);
        if (p < end && *p == '\n') p++;

        /* Skip empty / comment / bare-CR lines */
        if (len == 0 || line_start[0] == '#')
            continue;

        size_t actual_len = len;
        if (actual_len > 0 && line_start[actual_len - 1] == '\r')
            actual_len--;
        if (actual_len == 0)
            continue;

        /* Skip UTF-8 BOM on the very first line */
        if (line_start == dict->data && actual_len >= 3 &&
            (unsigned char)line_start[0] == 0xef &&
            (unsigned char)line_start[1] == 0xbb &&
            (unsigned char)line_start[2] == 0xbf) {
            line_start += 3;
            actual_len -= 3;
            if (actual_len == 0) continue;
        }

        int is_indented = (line_start[0] == ' ' || line_start[0] == '\t');

        if (!is_indented) {
            /* ── headword line ── */
            if (in_def) {
                /* We were reading a definition block → flush previous group */
                FLUSH_HEADWORDS();
            }

            /* Skip DSL header macros like {{...}} */
            if (line_start[0] == '{' && actual_len > 1 && line_start[1] == '{')
                continue;

            /* Grow the headword array if needed */
            if (hw_count >= hw_cap) {
                hw_cap = hw_cap == 0 ? 16 : hw_cap * 2;
                hws = realloc(hws, hw_cap * sizeof(HwSpan));
            }
            hws[hw_count].offset = (size_t)(line_start - dict->data);
            hws[hw_count].length = actual_len;
            hw_count++;

            /* Tentatively set def_offset to right after this line */
            def_offset = (size_t)(p - dict->data);
            def_len    = 0;
        } else {
            /* ── definition line (indented) ── */
            in_def  = 1;
            def_len = (size_t)(p - dict->data) - def_offset;
        }
    }

    /* Final flush */
    FLUSH_HEADWORDS();
    #undef FLUSH_HEADWORDS

    free(hws);
    printf("[DEBUG] parse_dsl_into_tree: indexed %d headwords.\n", word_count);
}

static size_t convert_utf16le_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len) {
        uint32_t wc = in_buf[in] | (in_buf[in+1] << 8);
        in += 2;
        if (wc >= 0xD800 && wc <= 0xDBFF && in + 1 < in_len) {
            uint32_t wc2 = in_buf[in] | (in_buf[in+1] << 8);
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                in += 2;
                wc = 0x10000 + ((wc & 0x3FF) << 10) + (wc2 & 0x3FF);
            }
        }
        if (wc < 0x80) { out_buf[out++] = wc; }
        else if (wc < 0x800) {
            out_buf[out++] = 0xC0 | (wc >> 6);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}

static size_t convert_utf16be_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len) {
        uint32_t wc = (in_buf[in] << 8) | in_buf[in+1];
        in += 2;
        if (wc >= 0xD800 && wc <= 0xDBFF && in + 1 < in_len) {
            uint32_t wc2 = (in_buf[in] << 8) | in_buf[in+1];
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                in += 2;
                wc = 0x10000 + ((wc & 0x3FF) << 10) + (wc2 & 0x3FF);
            }
        }
        if (wc < 0x80) { out_buf[out++] = wc; }
        else if (wc < 0x800) {
            out_buf[out++] = 0xC0 | (wc >> 6);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}

DictMmap* dict_mmap_open(const char *path) {
    if (!path) return NULL;
    size_t path_len = strlen(path);
    if (path_len > 4 && strcasecmp(path + path_len - 4, ".mdx") == 0) {
        fprintf(stderr, "MDX decompression mapping is currently in Phase 2 Development.\n");
        return NULL;
    }

    DictMmap *dict = (DictMmap*)calloc(1, sizeof(DictMmap));
    dict->fd = -1;

    printf("Loading Dictionary: %s\n", path);
    gzFile gz = gzopen(path, "rb");
    if (!gz) {
        free(dict);
        return NULL;
    }

    // Determine encoding by reading first 4 bytes
    unsigned char bom[4];
    int bom_len = gzread(gz, bom, 4);
    if (bom_len < 2) {
        gzclose(gz);
        free(dict);
        return NULL;
    }

    int is_utf16le = 0;
    int is_utf16be = 0;
    int copy_offset = 0;
    
    if (bom[0] == 0xFF && bom[1] == 0xFE) {
        is_utf16le = 1;
        copy_offset = 2;
    } else if (bom[0] == 0xFE && bom[1] == 0xFF) {
        is_utf16be = 1;
        copy_offset = 2;
    } else if (bom_len >= 4 && bom[0] != 0 && bom[1] == 0 && bom[2] != 0 && bom[3] == 0) {
        is_utf16le = 1;
        copy_offset = 0;
    } else if (bom_len >= 4 && bom[0] == 0 && bom[1] != 0 && bom[2] == 0 && bom[3] != 0) {
        is_utf16be = 1;
        copy_offset = 0;
    } else if (bom_len >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        copy_offset = 3;
    }

    dict->tmp_file = tmpfile();
    if (!dict->tmp_file) {
        gzclose(gz);
        free(dict);
        return NULL;
    }
    
    // Write whatever we read past the BOM
    if (bom_len > copy_offset) {
        if (is_utf16le) {
            unsigned char out[10];
            size_t olen = convert_utf16le_to_utf8(bom + copy_offset, bom_len - copy_offset, out);
            fwrite(out, 1, olen, dict->tmp_file);
        } else if (is_utf16be) {
            unsigned char out[10];
            size_t olen = convert_utf16be_to_utf8(bom + copy_offset, bom_len - copy_offset, out);
            fwrite(out, 1, olen, dict->tmp_file);
        } else {
            fwrite(bom + copy_offset, 1, bom_len - copy_offset, dict->tmp_file);
        }
    }

    unsigned char in_buf[65536];
    unsigned char out_buf[65536 * 2]; // UTF-8 can be larger than UTF-16
    
    // For UTF-16, we must handle boundary bytes gracefully (in case a chunk drops precisely between 2 framing bytes).
    unsigned char pending_byte = 0;
    int has_pending = 0;
    int bytes_read;
    
    while ((bytes_read = gzread(gz, in_buf + has_pending, 65536 - has_pending)) > 0) {
        int total = bytes_read + has_pending;
        size_t process_len = total;
        
        if ((is_utf16le || is_utf16be) && (total % 2 != 0)) {
            pending_byte = in_buf[total - 1];
            has_pending = 1;
            process_len = total - 1;
        } else {
            has_pending = 0;
        }

        if (process_len > 0) {
            if (is_utf16le) {
                size_t olen = convert_utf16le_to_utf8(in_buf, process_len, out_buf);
                fwrite(out_buf, 1, olen, dict->tmp_file);
            } else if (is_utf16be) {
                size_t olen = convert_utf16be_to_utf8(in_buf, process_len, out_buf);
                fwrite(out_buf, 1, olen, dict->tmp_file);
            } else {
                fwrite(in_buf, 1, process_len, dict->tmp_file);
            }
        }
        
        if (has_pending) {
            in_buf[0] = pending_byte;
        }
    }
    gzclose(gz);
    fflush(dict->tmp_file);
    
    dict->fd = fileno(dict->tmp_file);
    
    struct stat st;
    if (fstat(dict->fd, &st) < 0 || st.st_size == 0) {
        fclose(dict->tmp_file);
        free(dict);
        return NULL;
    }
    dict->size = st.st_size;
    
    void *map = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);
    if (map == MAP_FAILED) {
        fclose(dict->tmp_file);
        free(dict);
        return NULL;
    }
    
    dict->data = (const char*)map;
    printf("[DEBUG] First 256 bytes mapped (size: %zu):\n%.256s\n", dict->size, dict->data);
    dict->index = splay_tree_new(dict->data, dict->size);
    parse_dsl_into_tree(dict);
    
    return dict;
}

void dict_mmap_close(DictMmap *dict) {
    if (dict) {
        splay_tree_free(dict->index);
        if (dict->data) munmap((void*)dict->data, dict->size);
        if (dict->tmp_file) fclose(dict->tmp_file);
        else if (dict->fd >= 0) close(dict->fd);
        if (dict->resource_dir) free(dict->resource_dir);
        free(dict);
    }
}
