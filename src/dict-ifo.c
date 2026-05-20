/* dict-ifo.c — StarDict .ifo + .idx(.gz) + .dict(.dz) parser
 *
 * This keeps article bytes structured in the on-disk cache instead of
 * flattening entries into line-based text. That preserves multiline content,
 * typed resources, and HTML articles more faithfully.
 */

#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <utime.h>
#include <fcntl.h>
#include <glib.h>
#include <ctype.h>
#include "dict-cache.h"
#include "dict-cache-builder.h"
#include "dict-chunked.h"
#include "settings.h"
#include "dictzip.h"

/* IFO uses multi-source cache validation since it has .ifo + .idx + .dict */

static gboolean is_cache_valid_for_sources(const char *cache_path, const char **sources, size_t source_count) {
    struct stat cache_st;
    if (stat(cache_path, &cache_st) != 0) {
        return FALSE;
    }

    for (size_t i = 0; i < source_count; i++) {
        struct stat src_st;
        if (stat(sources[i], &src_st) != 0 || cache_st.st_mtime < src_st.st_mtime) {
            return FALSE;
        }
    }

    return TRUE;
}

/* sync_cache_mtime: Use dict_cache_sync_mtime from dict-cache.h instead */


static uint32_t read_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read_u64be(const unsigned char *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | p[7];
}

static gboolean ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return FALSE;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static char *find_existing_sibling(const char *base_path, const char * const *suffixes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        char *candidate = g_strconcat(base_path, suffixes[i], NULL);
        if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
            return candidate;
        }
        g_free(candidate);
    }

    return NULL;
}

static char *find_stardict_resource_dir(const char *ifo_path) {
    size_t base_len = strlen(ifo_path) - 4;
    char *base = g_strndup(ifo_path, base_len);
    const char *suffixes[] = { ".files", ".dict.files", ".ifo.files" };
    char *result = find_existing_sibling(base, suffixes, G_N_ELEMENTS(suffixes));
    g_free(base);
    return result;
}

static gboolean load_file_bytes_plain(const char *path, unsigned char **data_out, size_t *size_out) {
    gboolean ok = FALSE;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return FALSE;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return FALSE;
    }

    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return FALSE;
    }

    unsigned char *data = g_malloc(size > 0 ? (size_t)size : 1);
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        g_free(data);
        fclose(f);
        return FALSE;
    }

    *data_out = data;
    *size_out = (size_t)size;
    ok = TRUE;
    fclose(f);
    return ok;
}

static gboolean load_file_bytes_gzip(const char *path, unsigned char **data_out, size_t *size_out) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) {
        return FALSE;
    }

    struct stat st;
    int64_t total_src = (stat(path, &st) == 0) ? st.st_size : 1;
    int64_t total_transferred = 0;

    size_t cap = 1024 * 1024;
    size_t len = 0;
    unsigned char *data = g_malloc(cap);
    unsigned char buf[65536];

    for (;;) {
        int n = gzread(gz, buf, sizeof(buf));
        total_transferred += n;
        if (total_src > 0 && n > 0 && (total_transferred % (1024 * 1024)) == 0) {
            /* Heuristic: we report some progress for loading. 
             * Since there are multiple files, we take only a small slice. */
            settings_scan_progress_notify(path, (int)(total_transferred * 15 / total_src));
        }
        if (n < 0) {
            g_free(data);
            gzclose(gz);
            return FALSE;
        }
        if (n == 0) {
            break;
        }

        if (len + (size_t)n > cap) {
            while (len + (size_t)n > cap) {
                cap *= 2;
            }
            data = g_realloc(data, cap);
        }

        memcpy(data + len, buf, (size_t)n);
        len += (size_t)n;
    }

    gzclose(gz);
    *data_out = data;
    *size_out = len;
    return TRUE;
}

static gboolean load_file_bytes_auto(const char *path, unsigned char **data_out, size_t *size_out) {
    if (ends_with_ci(path, ".gz") || ends_with_ci(path, ".dz")) {
        return load_file_bytes_gzip(path, data_out, size_out);
    }
    return load_file_bytes_plain(path, data_out, size_out);
}

static void append_html_escaped_text(GString *out, const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        switch (data[i]) {
            case '&':
                g_string_append(out, "&amp;");
                break;
            case '<':
                g_string_append(out, "&lt;");
                break;
            case '>':
                g_string_append(out, "&gt;");
                break;
            case '"':
                g_string_append(out, "&quot;");
                break;
            case '\n':
                g_string_append(out, "<br/>");
                break;
            case '\r':
                break;
            default:
                g_string_append_c(out, data[i]);
                break;
        }
    }
}

static void append_stardict_resource_html(GString *article, char type, const char *data, size_t size) {
    while (size > 0 && data[size - 1] == '\0') {
        size--;
    }

    switch (type) {
        case 'h':
        case 'g':
        case 'x':
            g_string_append_len(article, data, size);
            break;
        case 'm':
        case 'l':
        case 't':
        case 'y':
        case 'w':
            append_html_escaped_text(article, data, size);
            break;
        default:
            append_html_escaped_text(article, data, size);
            break;
    }
}

static gboolean append_stardict_article(GString *article,
                                        const unsigned char *data,
                                        size_t size,
                                        const char *sametypesequence) {
    const unsigned char *ptr = data;
    size_t remaining = size;

    if (sametypesequence && *sametypesequence) {
        size_t seq_len = strlen(sametypesequence);

        for (size_t i = 0; i < seq_len && remaining > 0; i++) {
            char type = sametypesequence[i];
            gboolean last = (i + 1 == seq_len);

            if (islower((unsigned char)type)) {
                size_t entry_size = 0;
                if (last) {
                    entry_size = remaining;
                } else {
                    while (entry_size < remaining && ptr[entry_size] != '\0') {
                        entry_size++;
                    }
                    if (entry_size == remaining) {
                        return FALSE;
                    }
                }

                append_stardict_resource_html(article, type, (const char *)ptr, entry_size);
                ptr += entry_size;
                remaining -= entry_size;

                if (!last && remaining > 0) {
                    ptr++;
                    remaining--;
                }
            } else if (isupper((unsigned char)type)) {
                size_t entry_size = 0;
                if (last) {
                    entry_size = remaining;
                } else {
                    if (remaining < 4) {
                        return FALSE;
                    }
                    entry_size = read_u32be(ptr);
                    ptr += 4;
                    remaining -= 4;
                    if (entry_size > remaining) {
                        return FALSE;
                    }
                }

                append_stardict_resource_html(article, type, (const char *)ptr, entry_size);
                ptr += entry_size;
                remaining -= entry_size;
            } else {
                return FALSE;
            }
        }

        return TRUE;
    }

    while (remaining > 0) {
        char type = (char)*ptr++;
        remaining--;

        if (islower((unsigned char)type)) {
            size_t entry_size = 0;
            while (entry_size < remaining && ptr[entry_size] != '\0') {
                entry_size++;
            }
            if (entry_size == remaining) {
                return FALSE;
            }

            append_stardict_resource_html(article, type, (const char *)ptr, entry_size);
            ptr += entry_size + 1;
            remaining -= entry_size + 1;
        } else if (isupper((unsigned char)type)) {
            if (remaining < 4) {
                return FALSE;
            }

            uint32_t entry_size = read_u32be(ptr);
            ptr += 4;
            remaining -= 4;
            if (entry_size > remaining) {
                return FALSE;
            }

            append_stardict_resource_html(article, type, (const char *)ptr, entry_size);
            ptr += entry_size;
            remaining -= entry_size;
        } else {
            return FALSE;
        }
    }

    return TRUE;
}

char* stardict_transcode_article(const char *raw_data, size_t size, const char *sametypesequence) {
    if (!raw_data || size == 0) return NULL;
    GString *article = g_string_new("");
    if (!append_stardict_article(article, (const unsigned char*)raw_data, size, sametypesequence)) {
        g_string_assign(article, "");
        append_html_escaped_text(article, raw_data, size);
    }
    return g_string_free(article, FALSE);
}

static int parse_ifo_metadata(const char *ifo_path,
                              uint32_t *wordcount,
                              uint32_t *idxfilesize,
                              int *idxoffsetbits,
                              char *sametypesequence,
                              size_t sts_len,
                              char **bookname) {
    FILE *f = fopen(ifo_path, "r");
    if (!f) return -1;

    char line[1024];
    *wordcount = 0;
    *idxfilesize = 0;
    *idxoffsetbits = 32;
    sametypesequence[0] = '\0';
    if (bookname) *bookname = NULL;

    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    if (strncmp(line, "StarDict's dict ifo file", 24) != 0) {
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        if (strncmp(line, "wordcount=", 10) == 0) {
            *wordcount = (uint32_t)g_ascii_strtoull(line + 10, NULL, 10);
        } else if (strncmp(line, "idxfilesize=", 12) == 0) {
            *idxfilesize = (uint32_t)g_ascii_strtoull(line + 12, NULL, 10);
        } else if (strncmp(line, "sametypesequence=", 17) == 0) {
            g_strlcpy(sametypesequence, line + 17, sts_len);
        } else if (strncmp(line, "bookname=", 9) == 0 && bookname) {
            *bookname = g_strdup(line + 9);
        } else if (strncmp(line, "idxoffsetbits=", 14) == 0) {
            *idxoffsetbits = atoi(line + 14);
        }
    }

    fclose(f);
    return 0;
}

static gboolean build_stardict_cache(DictCacheBuilder *builder,
                                     const unsigned char *idx_data,
                                     size_t idx_size,
                                     size_t dict_raw_len,
                                     int idxoffsetbits,
                                     TreeEntry **entries_out,
                                     size_t *entry_count_out,
                                     const char *path, volatile gint *cancel_flag, gint expected) {
    const unsigned char *ip = idx_data;
    const unsigned char *ie = idx_data + idx_size;
    size_t cap = 4096;
    size_t count = 0;
    TreeEntry *entries = g_malloc(cap * sizeof(TreeEntry));

    int offset_size = (idxoffsetbits == 64) ? 8 : 4;
    int size_size = 4;
    int entry_size = offset_size + size_size;
    
    /* Heuristic to detect entry size (8, 12, or 16 bytes) */
    if (idxoffsetbits == 64) {
        const unsigned char *p = idx_data;
        while (p < ie && *p != '\0') p++;
        if (p < ie) {
            p++; /* Skip NULL */
            /* Try to guess entry size by looking for the next word's NULL terminator.
             * We check 12, 16, and 8 (though 8 is unlikely for 64-bit offset). */
            int possible_sizes[] = { 16, 12, 8 };
            for (int j = 0; j < 3; j++) {
                int s = possible_sizes[j];
                if (p + s < ie) {
                    /* If we assume size 's', the next word starts at p + s.
                     * Let's see if it looks like a valid word (NULL terminated soon). */
                    const unsigned char *p2 = p + s;
                    int k = 0;
                    while (k < 100 && p2 + k < ie && p2[k] != '\0') k++;
                    if (k < 100 && p2 + k < ie && p2[k] == '\0') {
                        /* Found a NULL within 100 bytes! This is a strong signal. */
                        entry_size = s;
                        offset_size = (s == 8) ? 4 : 8;
                        size_size = s - offset_size;
                        break;
                    }
                }
            }
        }
    }
    
    /* Some 64-bit StarDict files (like GCIDE) store 32-bit Big Endian values
     * at the START of 8-byte slots. We detect this by checking the first few entries. */
    gboolean offset_is_shifted = FALSE;
    gboolean size_is_shifted = FALSE;
    if (offset_size == 8 || size_size == 8) {
        const unsigned char *p = idx_data;
        for (int k = 0; k < 100 && p < ie; k++) {
            while (p < ie && *p != '\0') p++;
            if (p >= ie) break;
            p++; /* word\0 */
            if (p + entry_size > ie) break;
            
            if (offset_size == 8 && !offset_is_shifted) {
                uint32_t high = read_u32be(p);
                uint32_t low = read_u32be(p + 4);
                if (high > 0 && low == 0 && high <= (uint32_t)dict_raw_len) {
                    offset_is_shifted = TRUE;
                }
            }
            if (size_size == 8 && !size_is_shifted) {
                uint32_t high = read_u32be(p + offset_size);
                uint32_t low = read_u32be(p + offset_size + 4);
                if (high > 0 && low == 0 && high <= (uint32_t)dict_raw_len) {
                    size_is_shifted = TRUE;
                }
            }
            p += entry_size;
        }
    }

    size_t last_notified_idx = 0;
    size_t notify_interval = idx_size / 20;
    if (notify_interval < 1024) notify_interval = 1024;

    while (ip < ie) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        
        size_t current_pos = ip - idx_data;
        if (current_pos - last_notified_idx > notify_interval) {
            int pct = 30 + (int)(current_pos * 65 / idx_size); /* 30% to 95% for indexing */
            settings_scan_progress_notify(path, pct);
            last_notified_idx = current_pos;
        }

        const unsigned char *hw_start = ip;
        while (ip < ie && *ip != '\0') ip++;
        if (ip >= ie) {
            break;
        }

        size_t hw_len = ip - hw_start;
        ip++;

        if (ip + entry_size > ie) {
            break;
        }

        uint64_t def_offset;
        if (offset_size == 8) {
            def_offset = offset_is_shifted ? (uint64_t)read_u32be(ip) : read_u64be(ip);
        } else {
            def_offset = (uint64_t)read_u32be(ip);
        }
        
        uint64_t def_size;
        if (size_size == 8) {
            def_size = size_is_shifted ? (uint64_t)read_u32be(ip + offset_size) : read_u64be(ip + offset_size);
        } else {
            def_size = (uint64_t)read_u32be(ip + offset_size);
        }
        ip += entry_size;

        if (def_offset > (uint64_t)dict_raw_len || def_size > (uint64_t)dict_raw_len - def_offset) {
            continue;
        }

        uint64_t hw_off = 0;
        dict_cache_builder_add_headword(builder, (const char *)hw_start, hw_len, &hw_off);

        if (count == cap) {
            cap *= 2;
            entries = g_realloc(entries, cap * sizeof(TreeEntry));
        }

        entries[count].h_off = hw_off;
        entries[count].h_len = hw_len;
        entries[count].d_off = def_offset;
        entries[count].d_len = def_size;
        count++;
    }

    *entries_out = entries;
    *entry_count_out = count;
    return TRUE;
}

static DictMmap *open_cached_stardict(const char *cache_path, char *bookname, char *resource_dir, const char *dict_path) {
    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) {
        g_free(bookname);
        g_free(resource_dir);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 16) {
        close(fd);
        g_free(bookname);
        g_free(resource_dir);
        return NULL;
    }

    const char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        g_free(bookname);
        g_free(resource_dir);
        return NULL;
    }

    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = fd;
    close(dict->fd);
    dict->fd = -1;
    dict->data = data;
    dict->size = st.st_size;
    dict->name = bookname;
    dict->resource_dir = resource_dir;
    dict->index = flat_index_open(dict->data, dict->size);
    if (dict_cache_is_compressed(dict->data, dict->size)) {
        const DictCacheHeader *header = (const DictCacheHeader*)dict->data;
        dict->is_compressed = TRUE;
        if (header->chunk_count == 0) {
            /* Index-only source-backed cache */
            if (g_str_has_suffix(dict_path, ".dz") || g_str_has_suffix(dict_path, ".DZ")) {
                dict->source_dz = dictzip_open(dict_path);
            } else {
                dict->source_fd = open(dict_path, O_RDONLY);
                if (dict->source_fd >= 0) {
                    struct stat s_st;
                    fstat(dict->source_fd, &s_st);
                    dict->source_size = s_st.st_size;
                    dict->source_mmap = mmap(NULL, dict->source_size, PROT_READ, MAP_SHARED, dict->source_fd, 0);
                    close(dict->source_fd);
                    dict->source_fd = -1;
                }
            }
            if (header->stardict_sts[0] != '\0') {
                dict->stardict_sts = g_strdup(header->stardict_sts);
            }
        } else {
            dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, header);
        }
    }

    /* Validate the index loaded from cache */
    if (dict->index && dict->index->count > 0) {
        if (!flat_index_validate(dict->index)) {
            fprintf(stderr, "[IFO] Cache index validation failed for %s — rebuilding index.\n", cache_path);
            flat_index_close(dict->index);
            if (dict->chunk_reader) {
                dict_chunk_reader_free(dict->chunk_reader);
                dict->chunk_reader = NULL;
                dict->is_compressed = FALSE;
            }
            dict->index = g_new0(FlatIndex, 1);
            dict->index->mmap_data = dict->data;
            dict->index->mmap_size = dict->size;
        }
    }

    return dict;
}

DictMmap* parse_stardict(const char *ifo_path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    fprintf(stderr, "[IFO] Loading StarDict: %s\n", ifo_path);

    uint32_t wordcount = 0;
    uint32_t idxfilesize = 0;
    int idxoffsetbits = 32;
    char sametypesequence[32];
    char *bookname = NULL;

    if (parse_ifo_metadata(ifo_path, &wordcount, &idxfilesize, &idxoffsetbits,
                           sametypesequence, sizeof(sametypesequence), &bookname) != 0) {
        fprintf(stderr, "[IFO] Failed to parse .ifo: %s\n", ifo_path);
        g_free(bookname);
        return NULL;
    }

    size_t base_len = strlen(ifo_path) - 4;
    char *base = g_strndup(ifo_path, base_len);
    const char *idx_suffixes[] = { ".idx", ".idx.gz", ".idx.dz", ".IDX", ".IDX.GZ", ".IDX.DZ" };
    const char *dict_suffixes[] = { ".dict.dz", ".dict", ".DICT.DZ", ".DICT" };
    char *idx_path = find_existing_sibling(base, idx_suffixes, G_N_ELEMENTS(idx_suffixes));
    char *dict_path = find_existing_sibling(base, dict_suffixes, G_N_ELEMENTS(dict_suffixes));
    char *resource_dir = find_stardict_resource_dir(ifo_path);
    g_free(base);

    if (!idx_path || !dict_path) {
        fprintf(stderr, "[IFO] Missing companion files for %s\n", ifo_path);
        g_free(bookname);
        g_free(idx_path);
        g_free(dict_path);
        g_free(resource_dir);
        return NULL;
    }

    dict_cache_ensure_dir();
    char *cache_path = dict_cache_path_for(ifo_path);
    const char *sources[] = { ifo_path, idx_path, dict_path };

    if (is_cache_valid_for_sources(cache_path, sources, G_N_ELEMENTS(sources))) {
        DictMmap *cached = open_cached_stardict(cache_path, bookname, resource_dir, dict_path);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        return cached;
    }

    /* RAM-saving strategy:
     * - Plain .dict  → mmap(MAP_PRIVATE) so the OS pages it lazily (zero heap cost).
     * - .dict.dz files > STARDICT_INDEX_ONLY_DZ_THRESHOLD → index-only mode:
     *   we store only headword offsets in the cache and open the .dz file via
     *   dictzip random-access at runtime.  This avoids decompressing hundreds of
     *   MB into RAM during a cold-cache rebuild.
     * - .dict.dz files <= threshold → decompress fully as before (small dicts).
     */
#define STARDICT_INDEX_ONLY_DZ_THRESHOLD (64ULL * 1024ULL * 1024ULL)  /* 64 MB compressed */

    const unsigned char *dict_raw = NULL;
    size_t dict_raw_len = 0;
    int    dict_raw_fd  = -1;   /* valid when we mmap'd a plain .dict */
    gboolean dict_is_heap = FALSE; /* TRUE when dict_raw was g_malloc'd (small .dz) */
    gboolean index_only_mode = FALSE;

    unsigned char *idx_data = NULL;
    size_t idx_size = 0;

    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        return NULL;
    }

    /* Load .idx (always small enough to heap-load) */
    if (!load_file_bytes_auto(idx_path, &idx_data, &idx_size)) {
        fprintf(stderr, "[IFO] Failed reading .idx for %s\n", ifo_path);
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        return NULL;
    }

    /* Load .dict / .dict.dz as efficiently as possible */
    gboolean dict_is_compressed = ends_with_ci(dict_path, ".dz") ||
                                   ends_with_ci(dict_path, ".gz");
    if (!dict_is_compressed) {
        /* Plain .dict → mmap(MAP_PRIVATE), zero heap cost */
        dict_raw_fd = open(dict_path, O_RDONLY);
        if (dict_raw_fd >= 0) {
            struct stat ds;
            if (fstat(dict_raw_fd, &ds) == 0 && ds.st_size > 0) {
                void *m = mmap(NULL, (size_t)ds.st_size, PROT_READ, MAP_PRIVATE,
                               dict_raw_fd, 0);
                if (m != MAP_FAILED) {
                    dict_raw     = (const unsigned char *)m;
                    dict_raw_len = (size_t)ds.st_size;
                    /* Hint the kernel: sequential single-pass access during build */
                    madvise((void*)dict_raw, dict_raw_len, MADV_SEQUENTIAL);
                }
            }
            if (!dict_raw) {
                close(dict_raw_fd);
                dict_raw_fd = -1;
            }
        }
        if (!dict_raw) {
            /* Fallback: heap load */
            unsigned char *tmp = NULL;
            if (load_file_bytes_plain(dict_path, &tmp, &dict_raw_len)) {
                dict_raw      = tmp;
                dict_is_heap  = TRUE;
            }
        }
    } else {
        /* Compressed .dict.dz — check size before decompressing */
        struct stat dz_st;
        if (stat(dict_path, &dz_st) == 0 &&
            (guint64)dz_st.st_size > STARDICT_INDEX_ONLY_DZ_THRESHOLD) {
            /* Large compressed dict → index-only: store offsets only, open via
             * dictzip at runtime.  dict_raw stays NULL intentionally. */
            index_only_mode = TRUE;
            fprintf(stderr,
                    "[IFO] %s: .dict.dz is %.1f MB (> %.0f MB threshold) — "
                    "using index-only mode (dictzip runtime access)\n",
                    ifo_path,
                    (double)dz_st.st_size / (1024.0 * 1024.0),
                    (double)STARDICT_INDEX_ONLY_DZ_THRESHOLD / (1024.0 * 1024.0));
            /* We need dict_raw_len so entry offset validation works. Approximate
             * via wordcount × avg_entry_size heuristic, or read header. */
            dict_raw_len = (size_t)dz_st.st_size * 5; /* conservative 5× expansion */
        } else {
            /* Small compressed dict → decompress to heap as before */
            unsigned char *tmp = NULL;
            if (load_file_bytes_gzip(dict_path, &tmp, &dict_raw_len)) {
                dict_raw     = tmp;
                dict_is_heap = TRUE;
            }
        }
    }

    if (!dict_raw && !index_only_mode) {
        fprintf(stderr, "[IFO] Failed to load .dict for %s\n", ifo_path);
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        g_free(idx_data);
        return NULL;
    }

    /* Heuristic to auto-detect entry size (8, 12, or 16 bytes) if it's missing or unreliable.
     * Some dictionaries like GCIDE have 64-bit offsets and 64-bit sizes (16 bytes total)
     * even without the idxoffsetbits=64 flag. */
    if (idxfilesize > 0 && wordcount > 0) {
        int sizes[] = { 16, 12, 8 };
        int best_size = (idxoffsetbits == 64) ? 12 : 8;
        int max_score = -1;

        for (int j = 0; j < 3; j++) {
            int s = sizes[j];
            const unsigned char *p = idx_data;
            int score = 0;
            int entries_checked = 0;
            
            for (int k = 0; k < 20 && p < idx_data + idx_size; k++) {
                while (p < idx_data + idx_size && *p != '\0') p++;
                if (p >= idx_data + idx_size) break;
                p++; /* word\0 */
                if (p + s > idx_data + idx_size) break;
                
                /* After skip s bytes, we should be at the start of a word.
                 * A word should be NULL terminated within a reasonable distance
                 * and contain printable characters. */
                const unsigned char *p2 = p + s;
                int len = 0;
                while (len < 200 && p2 + len < idx_data + idx_size && p2[len] != '\0') {
                    if (!isprint(p2[len]) && (p2[len] < 0x80)) {
                        /* Non-printable ASCII is a bad sign for a word. */
                        score -= 10;
                        break;
                    }
                    len++;
                }
                if (p2 + len < idx_data + idx_size && p2[len] == '\0') {
                    score += 5;
                    if (len > 0) score += 2; /* non-empty word is better */
                } else {
                    score -= 5;
                }
                p = p2;
                entries_checked++;
            }
            if (entries_checked > 0 && score >= max_score) {
                /* If scores are tied, prefer the larger size (16 > 12 > 8) */
                max_score = score;
                best_size = s;
            }
        }
        
        if (best_size == 16) {
            printf("[IFO] Auto-detected 16-byte entries (64-bit offset + 64-bit size) for %s\n", ifo_path);
            idxoffsetbits = 64;
        } else if (best_size == 12) {
            printf("[IFO] Auto-detected 12-byte entries (64-bit offset + 32-bit size) for %s\n", ifo_path);
            idxoffsetbits = 64;
        } else if (best_size == 8 && idxoffsetbits == 64) {
            /* If it was supposed to be 64 but heuristic says 8, trust the heuristic. */
            idxoffsetbits = 32;
        }
    }

    if (!dict_cache_prepare_target_path(cache_path, (guint64)dict_raw_len + (guint64)idx_size)) {
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        if (dict_raw_fd >= 0) { munmap((void*)dict_raw, dict_raw_len); close(dict_raw_fd); }
        else if (dict_is_heap) g_free((void*)dict_raw);
        g_free(idx_data);
        return NULL;
    }
    DictCacheBuilder *builder = dict_cache_builder_new(cache_path, wordcount);
    if (!builder) {
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        if (dict_raw_fd >= 0) { munmap((void*)dict_raw, dict_raw_len); close(dict_raw_fd); }
        else if (dict_is_heap) g_free((void*)dict_raw);
        g_free(idx_data);
        return NULL;
    }

    TreeEntry *entries = NULL;
    size_t entry_count = 0;
    settings_scan_progress_notify(ifo_path, 30);
    gboolean built = build_stardict_cache(builder, idx_data, idx_size, dict_raw_len,
                                          idxoffsetbits, &entries, &entry_count,
                                          ifo_path, cancel_flag, expected);

    if (built && entry_count > 0 && entries) {
        dict_cache_builder_flush(builder);

        /* Sort headwords using mmap(MAP_PRIVATE) of the partially-written cache —
         * this avoids a full malloc+fread copy (fix for RAM bloat). */
        int sort_fd = open(cache_path, O_RDONLY);
        if (sort_fd >= 0) {
            struct stat sort_st;
            if (fstat(sort_fd, &sort_st) == 0 && sort_st.st_size > 0) {
                void *sort_map = mmap(NULL, (size_t)sort_st.st_size, PROT_READ,
                                     MAP_PRIVATE, sort_fd, 0);
                if (sort_map != MAP_FAILED) {
                    flat_index_sort_entries(entries, entry_count,
                                           (const char *)sort_map,
                                           (size_t)sort_st.st_size);
                    munmap(sort_map, (size_t)sort_st.st_size);
                }
            }
            close(sort_fd);
        }

        dict_cache_builder_finalize_index_only(builder, entries, (uint64_t)entry_count, 0, sametypesequence);
    }
    dict_cache_builder_free(builder);
    dict_cache_sync_mtime(cache_path, sources, G_N_ELEMENTS(sources));

    g_free(entries);

    /* Release .dict source memory — method depends on how we loaded it */
    if (dict_raw_fd >= 0) {
        /* mmap'd plain .dict */
        if (dict_raw) munmap((void*)dict_raw, dict_raw_len);
        close(dict_raw_fd);
    } else if (dict_is_heap) {
        /* heap-alloc'd small .dz */
        g_free((void*)dict_raw);
    }
    /* index_only_mode: dict_raw == NULL, nothing to free */

    g_free(idx_data);
    g_free(idx_path);

    if (!built) {
        g_free(dict_path);
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        return NULL;
    }

    DictMmap *dict = open_cached_stardict(cache_path, bookname, resource_dir, dict_path);
    g_free(dict_path);
    g_free(cache_path);
    settings_scan_progress_notify(ifo_path, 100);
    return dict;
}
