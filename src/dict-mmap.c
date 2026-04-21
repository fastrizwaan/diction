#include "dict-mmap.h"
#include "flat-index.h"
#include "resource-reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <utime.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include "dict-cache.h"

/* insert_balanced removed — flat index uses sorted array + binary search */

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

// Cache directory helpers


static char *dsl_find_local_resource_dir(const char *path) {
    char *candidate = g_strconcat(path, ".files", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
        return candidate;
    }
    g_free(candidate);

    if (g_str_has_suffix(path, ".dz")) {
        char *without_dz = g_strndup(path, strlen(path) - 3);
        candidate = g_strconcat(without_dz, ".files", NULL);
        g_free(without_dz);
        if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
            return candidate;
        }
        g_free(candidate);
    } else if (g_str_has_suffix(path, ".dsl")) {
        candidate = g_strconcat(path, ".dz.files", NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
            return candidate;
        }
        g_free(candidate);
    }

    return NULL;
}

static char *dsl_find_resource_zip(const char *path) {
    char *candidate = g_strconcat(path, ".files.zip", NULL);
    if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
        return candidate;
    }
    g_free(candidate);

    if (g_str_has_suffix(path, ".dz")) {
        char *without_dz = g_strndup(path, strlen(path) - 3);
        candidate = g_strconcat(without_dz, ".files.zip", NULL);
        g_free(without_dz);
        if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
            return candidate;
        }
        g_free(candidate);
    } else if (g_str_has_suffix(path, ".dsl")) {
        candidate = g_strconcat(path, ".dz.files.zip", NULL);
        if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
            return candidate;
        }
        g_free(candidate);
    }

    return NULL;
}

static char *dsl_prepare_resource_dir(const char *path, ResourceReader **out_reader) {
    char *local_dir = dsl_find_local_resource_dir(path);
    if (local_dir) {
        return local_dir;
    }

    char *zip_path = dsl_find_resource_zip(path);
    if (!zip_path) {
        return NULL;
    }

    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
    const char *base = dict_cache_base_dir();
    char *resource_dir = g_build_filename(base, "diction", "resources", hash, NULL);
    g_free(hash);
    if (g_mkdir_with_parents(resource_dir, 0755) != 0) {
        g_free(resource_dir);
        g_free(zip_path);
        return NULL;
    }

    /* Phase 2: Lazy extraction — scan ZIP but don't extract.
     * Individual files will be extracted on demand by ResourceReader. */
    if (out_reader) {
        *out_reader = resource_reader_open_zip(zip_path, resource_dir);
    }

    g_free(zip_path);
    return resource_dir;
}

typedef struct {
    size_t offset;
    size_t length;
} HwSpan;

static size_t parse_dsl_into_entries(DictMmap *dict, TreeEntry **out_entries, size_t data_size, size_t data_start_offset) {
    const char *p   = dict->data + data_start_offset;
    const char *end = dict->data + data_size;

    /* Dynamic headword accumulator */
    HwSpan *hws     = NULL;
    size_t  hw_count = 0;
    size_t  hw_cap   = 0;

    size_t def_offset = 0;
    size_t def_len    = 0;
    int    in_def     = 0;
    size_t word_count = 0;

    TreeEntry *entries = NULL;
    size_t entry_cap = 0;

    /* Flush: collect every headword with the current def block */
    #define FLUSH_HEADWORDS() do {                                       \
        if (in_def && hw_count > 0 && def_len > 0) {                    \
            for (size_t _i = 0; _i < hw_count; _i++) {                  \
                if (word_count >= entry_cap) {                           \
                    entry_cap = (entry_cap == 0) ? 1024 : entry_cap * 2; \
                    entries = realloc(entries, entry_cap * sizeof(TreeEntry)); \
                }                                                        \
                entries[word_count].h_off = (int64_t)hws[_i].offset;     \
                entries[word_count].h_len = (uint64_t)hws[_i].length;    \
                entries[word_count].d_off = (int64_t)def_offset;         \
                entries[word_count].d_len = (uint64_t)def_len;           \
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
        if (len == 0) continue;
        
        if (line_start[0] == '#') {
            /* Support #NAME "Dictionary Name" header */
            if (len > 6 && strncasecmp(line_start, "#NAME", 5) == 0) {
                const char *val_start = line_start + 5;
                while (val_start < line_start + len && (*val_start == ' ' || *val_start == '\t' || *val_start == '\"')) val_start++;
                const char *val_end = line_start + len;
                while (val_end > val_start && (*(val_end - 1) == '\r' || *(val_end - 1) == '\"' || *(val_end - 1) == ' ' || *(val_end - 1) == '\t')) val_end--;
                
                if (val_end > val_start && !dict->name) {
                    dict->name = strndup(val_start, (size_t)(val_end - val_start));
                    printf("[DSL] Found dictionary name: %s\n", dict->name);
                }
            } else if (len > 16 && strncasecmp(line_start, "#INDEX_LANGUAGE", 15) == 0) {
                const char *val_start = line_start + 15;
                while (val_start < line_start + len && (*val_start == ' ' || *val_start == '\t' || *val_start == '\"')) val_start++;
                const char *val_end = line_start + len;
                while (val_end > val_start && (*(val_end - 1) == '\r' || *(val_end - 1) == '\"' || *(val_end - 1) == ' ' || *(val_end - 1) == '\t')) val_end--;
                if (val_end > val_start && !dict->source_lang) dict->source_lang = strndup(val_start, (size_t)(val_end - val_start));
            } else if (len > 19 && strncasecmp(line_start, "#CONTENTS_LANGUAGE", 18) == 0) {
                const char *val_start = line_start + 18;
                while (val_start < line_start + len && (*val_start == ' ' || *val_start == '\t' || *val_start == '\"')) val_start++;
                const char *val_end = line_start + len;
                while (val_end > val_start && (*(val_end - 1) == '\r' || *(val_end - 1) == '\"' || *(val_end - 1) == ' ' || *(val_end - 1) == '\t')) val_end--;
                if (val_end > val_start && !dict->target_lang) dict->target_lang = strndup(val_start, (size_t)(val_end - val_start));
            }
            continue;
        }

        size_t actual_len = len;
        if (actual_len > 0 && line_start[actual_len - 1] == '\r')
            actual_len--;
        if (actual_len == 0)
            continue;

        /* Skip UTF-8 BOM on the very first line */
        if (line_start == dict->data + data_start_offset && actual_len >= 3 &&
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

            /* Split headword by semicolon if it's not an HTML entity or escaped */
            const char *lptr = line_start;
            size_t sub_start = 0;
            for (size_t i = 0; i <= actual_len; i++) {
                if (i == actual_len || lptr[i] == ';') {
                    /* Check for backslash escape: \; */
                    if (lptr[i] == ';' && i > 0 && lptr[i-1] == '\\') {
                        int bs_count = 0;
                        for (int k = (int)i - 1; k >= 0 && lptr[k] == '\\'; k--) bs_count++;
                        if (bs_count % 2 != 0) continue; /* Escaped semicolon, don't split here */
                    }

                    /* Entity protection: don't split if this looks like &...; */
                    int is_entity = 0;
                    if (i > 0 && lptr[i] == ';') {
                        for (int j = (int)i - 1; j >= (int)sub_start && j >= (int)i - 10; j--) {
                            if (lptr[j] == '&') {
                                is_entity = 1;
                                break;
                            }
                            if (!g_ascii_isalnum(lptr[j]) && lptr[j] != '#') break;
                        }
                    }
                    if (is_entity && i < actual_len) continue;

                    size_t sub_off = sub_start;
                    size_t sub_len = i - sub_start;
                    
                    /* Trim whitespace for analysis */
                    while (sub_len > 0 && g_ascii_isspace(lptr[sub_off])) { sub_off++; sub_len--; }
                    while (sub_len > 0 && g_ascii_isspace(lptr[sub_off + sub_len - 1])) { sub_len--; }

                    if (sub_len > 0) {
                        /* Emoticon protection: If this sub-headword contains NO alphanumeric characters
                         * and it's NOT the first headword, reattach it to the previous one instead
                         * of creating a new entry. This preserves things like "Love Pat ;)" */
                        int has_alnum = 0;
                        for (size_t k = 0; k < sub_len; k++) {
                            if (g_ascii_isalnum(lptr[sub_off + k]) || (unsigned char)lptr[sub_off + k] >= 0x80) {
                                has_alnum = 1;
                                break;
                            }
                        }

                        if (hw_count > 0 && !has_alnum) {
                            /* Reattach to previous headword by extending its length.
                             * The new length covers everything from previous offset to end of current sub. */
                            hws[hw_count - 1].length = (size_t)(lptr + sub_off + sub_len - (dict->data + hws[hw_count - 1].offset));
                        } else {
                            /* Grow the headword array if needed */
                            if (hw_count >= hw_cap) {
                                hw_cap = hw_cap == 0 ? 16 : hw_cap * 2;
                                hws = realloc(hws, hw_cap * sizeof(HwSpan));
                            }
                            hws[hw_count].offset = (size_t)(lptr + sub_off - dict->data);
                            hws[hw_count].length = sub_len;
                            hw_count++;
                        }
                    }
                    sub_start = i + 1;
                }
            }

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
    if (out_entries) {
        /* Sort entries for binary search */
        if (entries && word_count > 0) {
            flat_index_sort_entries(entries, word_count, dict->data, dict->size);
        }
        *out_entries = entries;
    } else {
        free(entries);
    }
    printf("[DEBUG] parse_dsl_into_entries: indexed %zu headwords.\n", word_count);
    return word_count;
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

/* New signature accepts cancel flag and expected generation for cooperative cancellation. */
DictMmap* dict_mmap_open(const char *path, volatile gint *cancel_flag, gint expected) {
    if (!path) return NULL;
    size_t path_len = strlen(path);
    if (path_len > 4 && strcasecmp(path + path_len - 4, ".mdx") == 0) {
        fprintf(stderr, "MDX decompression mapping is currently in Phase 2 Development.\n");
        return NULL;
    }

    // Ensure cache directory exists
    dict_cache_ensure_dir();

    // Get cache path for this dictionary
    char *cache_path = dict_cache_path_for(path);
    gboolean cache_exists = (access(cache_path, F_OK) == 0);
    gboolean cache_valid = cache_exists && dict_cache_is_valid(cache_path, path);

    DictMmap *dict = (DictMmap*)calloc(1, sizeof(DictMmap));
    dict->fd = -1;
    dict->tmp_file = NULL;
    dict->source_dir = g_path_get_dirname(path);
    dict->resource_dir = dsl_prepare_resource_dir(path, &dict->resource_reader);

    if (cache_valid) {
        // Use cached version directly
        printf("Loading Dictionary from cache: %s\n", cache_path);
        dict->fd = open(cache_path, O_RDONLY);
        if (dict->fd < 0) {
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        struct stat st;
        if (fstat(dict->fd, &st) < 0 || st.st_size < 8) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }
        dict->size = st.st_size;

        void *map = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);
        if (map == MAP_FAILED) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        dict->data = (const char*)map;
        dict->index = flat_index_open(dict->data, dict->size);

        // Fast load: read index from end
        uint64_t count = *(uint64_t*)dict->data;
        int need_index = (count == 0);

        TreeEntry *entries = NULL;
        size_t index_size = 0;
        if (count > 0) {
            index_size = count * sizeof(TreeEntry);
            if (dict->size > index_size + 8) {
                entries = (TreeEntry*)(dict->data + (dict->size - index_size));

                /* Validate index entries to avoid using stale/corrupt caches
                 * (which can happen when on-disk cache formats change). If any
                 * entry points outside the data region, fall back to
                 * re-indexing by parsing the original file. */
                size_t data_region_end = dict->size - index_size; /* first byte of index */
                gboolean valid_index = TRUE;
                for (uint64_t i = 0; i < count; i++) {
                    int64_t h_off = entries[i].h_off;
                    uint64_t h_len = entries[i].h_len;
                    int64_t d_off = entries[i].d_off;
                    uint64_t d_len = entries[i].d_len;
                    /* Basic sanity checks */
                    if (h_off < 8 || (uint64_t)h_off >= data_region_end) { valid_index = FALSE; break; }
                    if (d_off < 8 || (uint64_t)d_off >= data_region_end) { valid_index = FALSE; break; }
                    if ((uint64_t)h_off + h_len > data_region_end) { valid_index = FALSE; break; }
                    if ((uint64_t)d_off + d_len > data_region_end) { valid_index = FALSE; break; }
                }
                if (valid_index) {
                    /* Index already loaded via flat_index_open — no insert needed */
                    printf("[DSL] Fast-loaded %lu entries from cache.\n", (unsigned long)count);
                    /* Ensure a name exists for UI (some caches omit it) */
                    if (!dict->name) {
                        char *base = g_path_get_basename(path);
                        dict->name = g_strdup(base);
                        g_free(base);
                    }
                } else {
                    fprintf(stderr, "[DSL] Cache index validation failed for %s — rebuilding index.\n", path);
                    need_index = 1;
                }
            } else {
                need_index = 1;
            }
        }

        if (need_index) {
            printf("[DSL] Cache exists but lacks index. Performing auto-upgrade...\n");
            TreeEntry *entries = NULL;
            size_t data_size = dict->size;
            if (count > 0) data_size = dict->size - index_size;
            size_t word_count = parse_dsl_into_entries(dict, &entries, data_size, 8);

            /* Check for cancellation before attempting to upgrade cache */
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                // abort and cleanup
                if (dict->fd >= 0) close(dict->fd);
                if (dict->tmp_file) fclose(dict->tmp_file);
                free(entries);
                free(dict);
                g_free(cache_path);
                return NULL;
            }

            if (word_count > 0 && entries) {
                // Re-open O_RDWR to upgrade cache
                int fd_rw = open(cache_path, O_RDWR);
                if (fd_rw >= 0) {
                    // Truncate to remove any old/stale index before appending new one
                    ftruncate(fd_rw, (off_t)data_size);

                    lseek(fd_rw, 0, SEEK_END);
                    write(fd_rw, entries, word_count * sizeof(TreeEntry));
                    lseek(fd_rw, 0, SEEK_SET);
                    uint64_t final_cnt = (uint64_t)word_count;
                    write(fd_rw, &final_cnt, 8);
                    close(fd_rw);

                    // Re-mmap the newly written file
                    munmap((void*)dict->data, dict->size);
                    close(dict->fd);
                    dict->fd = open(cache_path, O_RDONLY);
                    struct stat st_new;
                    fstat(dict->fd, &st_new);
                    dict->size = st_new.st_size;
                    dict->data = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);

                    // Reopen flat index with new data
                    flat_index_close(dict->index);
                    dict->index = flat_index_open(dict->data, dict->size);
                    printf("[DSL] Auto-upgraded cache for %s (%lu entries).\n", path, (unsigned long)word_count);
                }
                free(entries);
            }
        }
        
        close(dict->fd);
        dict->fd = -1;
    } else {
        // Need to extract and convert
        printf("Loading Dictionary: %s\n", path);
        gzFile gz = gzopen(path, "rb");
        if (!gz) {
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        // Open cache file for writing
        FILE *cache_file = fopen(cache_path, "wb");
        if (!cache_file) {
            gzclose(gz);
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        // Write placeholder for count
        uint64_t zero_count = 0;
        fwrite(&zero_count, 8, 1, cache_file);

        // Determine encoding by reading first 4 bytes
        unsigned char bom[4];
        int bom_len = gzread(gz, bom, 4);
        if (bom_len < 2) {
            gzclose(gz);
            fclose(cache_file);
            g_free(cache_path);
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

        // Write whatever we read past the BOM
        if (bom_len > copy_offset) {
            if (is_utf16le) {
                unsigned char out[10];
                size_t olen = convert_utf16le_to_utf8(bom + copy_offset, bom_len - copy_offset, out);
                fwrite(out, 1, olen, cache_file);
            } else if (is_utf16be) {
                unsigned char out[10];
                size_t olen = convert_utf16be_to_utf8(bom + copy_offset, bom_len - copy_offset, out);
                fwrite(out, 1, olen, cache_file);
            } else {
                fwrite(bom + copy_offset, 1, bom_len - copy_offset, cache_file);
            }
        }

        unsigned char in_buf[65536];
        unsigned char out_buf[65536 * 2];

        unsigned char pending_byte = 0;
        int has_pending = 0;
        int bytes_read;

        while ((bytes_read = gzread(gz, in_buf + has_pending, 65536 - has_pending)) > 0) {
            /* Honor cancellation request as soon as possible */
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                gzclose(gz);
                fclose(cache_file);
                unlink(cache_path);
                g_free(cache_path);
                free(dict);
                return NULL;
            }
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
                    fwrite(out_buf, 1, olen, cache_file);
                } else if (is_utf16be) {
                    size_t olen = convert_utf16be_to_utf8(in_buf, process_len, out_buf);
                    fwrite(out_buf, 1, olen, cache_file);
                } else {
                    fwrite(in_buf, 1, process_len, cache_file);
                }
            }

            if (has_pending) {
                in_buf[0] = pending_byte;
            }
        }
        gzclose(gz);
        fflush(cache_file);
        fclose(cache_file);

        // Now open the cached file
        dict->fd = open(cache_path, O_RDWR);
        if (dict->fd < 0) {
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        struct stat st;
        if (fstat(dict->fd, &st) < 0 || st.st_size == 0) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }
        dict->size = st.st_size;

        void *map = mmap(NULL, dict->size, PROT_READ | PROT_WRITE, MAP_SHARED, dict->fd, 0);
        if (map == MAP_FAILED) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        dict->data = (const char*)map;

        TreeEntry *entries = NULL;
        size_t word_count = parse_dsl_into_entries(dict, &entries, dict->size, 8);

        if (word_count > 0 && entries) {
            // Append entries to cache file
            lseek(dict->fd, 0, SEEK_END);
            write(dict->fd, entries, word_count * sizeof(TreeEntry));

            // Update count at beginning
            lseek(dict->fd, 0, SEEK_SET);
            uint64_t final_count = (uint64_t)word_count;
            write(dict->fd, &final_count, 8);

            free(entries);
        }

        // Sync cache mtime to match source (after all writes including index)
        {
            struct stat src_st;
            if (stat(path, &src_st) == 0) {
                struct utimbuf times;
                times.actime = src_st.st_mtime;
                times.modtime = src_st.st_mtime;
                utime(cache_path, &times);
            }
        }

        // Remap as read-only for final use
        munmap(map, dict->size);
        struct stat st_final;
        fstat(dict->fd, &st_final);
        dict->size = st_final.st_size;
        dict->data = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);
        close(dict->fd);
        dict->fd = -1;

        // Open flat index from the newly written cache
        dict->index = flat_index_open(dict->data, dict->size);
    }

    return dict;
}

void dict_mmap_close(DictMmap *dict) {
    if (dict) {
        flat_index_close(dict->index);
        resource_reader_close(dict->resource_reader);
        if (dict->data) munmap((void*)dict->data, dict->size);
        if (dict->fd >= 0) close(dict->fd);
        if (dict->name) free(dict->name);
        if (dict->source_dir) free(dict->source_dir);
        if (dict->resource_dir) free(dict->resource_dir);
        if (dict->mdx_stylesheet) free(dict->mdx_stylesheet);
        if (dict->source_lang) free(dict->source_lang);
        if (dict->target_lang) free(dict->target_lang);
        if (dict->icon_path) free(dict->icon_path);
        free(dict);
    }
}
