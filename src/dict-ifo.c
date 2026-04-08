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

static const char* get_cache_base_dir(void) {
    static const char *cache_dir = NULL;
    if (!cache_dir) cache_dir = g_get_user_cache_dir();
    return cache_dir;
}

static char* get_cache_dir_path(void) {
    const char *base = get_cache_base_dir();
    return g_build_filename(base, "diction", "dicts", NULL);
}

static char* get_cached_dict_path(const char *original_path) {
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, original_path, -1);
    const char *base = get_cache_base_dir();
    char *path = g_build_filename(base, "diction", "dicts", hash, NULL);
    g_free(hash);
    return path;
}

static gboolean ensure_cache_directory(void) {
    char *cache_dir = get_cache_dir_path();
    int ret = g_mkdir_with_parents(cache_dir, 0755);
    g_free(cache_dir);
    return ret == 0;
}

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

static void sync_cache_mtime(const char *cache_path, const char **sources, size_t source_count) {
    time_t latest = 0;

    for (size_t i = 0; i < source_count; i++) {
        struct stat src_st;
        if (stat(sources[i], &src_st) == 0 && src_st.st_mtime > latest) {
            latest = src_st.st_mtime;
        }
    }

    if (latest > 0) {
        struct utimbuf times;
        times.actime = latest;
        times.modtime = latest;
        utime(cache_path, &times);
    }
}

static uint32_t read_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
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

    size_t cap = 1024 * 1024;
    size_t len = 0;
    unsigned char *data = g_malloc(cap);
    unsigned char buf[65536];

    for (;;) {
        int n = gzread(gz, buf, sizeof(buf));
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
            g_string_append_len(article, data, size);
            break;
        case 'm':
        case 'l':
        case 'g':
        case 't':
        case 'y':
            append_html_escaped_text(article, data, size);
            break;
        case 'x':
            g_string_append(article, "<pre class=\"sdct_x\">");
            append_html_escaped_text(article, data, size);
            g_string_append(article, "</pre>");
            break;
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

static int parse_ifo_metadata(const char *ifo_path,
                              uint32_t *wordcount,
                              uint32_t *idxfilesize,
                              char *sametypesequence,
                              size_t sts_len,
                              char **bookname) {
    FILE *f = fopen(ifo_path, "r");
    if (!f) return -1;

    char line[1024];
    *wordcount = 0;
    *idxfilesize = 0;
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
        }
    }

    fclose(f);
    return 0;
}

static gboolean build_stardict_cache(FILE *cache_file,
                                     const unsigned char *idx_data,
                                     size_t idx_size,
                                     const unsigned char *dict_raw,
                                     size_t dict_raw_len,
                                     const char *sametypesequence,
                                     TreeEntry **entries_out,
                                     size_t *entry_count_out) {
    const unsigned char *ip = idx_data;
    const unsigned char *ie = idx_data + idx_size;
    size_t cap = 4096;
    size_t count = 0;
    TreeEntry *entries = g_new0(TreeEntry, cap);

    while (ip < ie) {
        const unsigned char *hw_start = ip;
        while (ip < ie && *ip != '\0') ip++;
        if (ip >= ie) {
            break;
        }

        size_t hw_len = ip - hw_start;
        ip++;

        if (ip + 8 > ie) {
            break;
        }

        uint32_t def_offset = read_u32be(ip);
        uint32_t def_size = read_u32be(ip + 4);
        ip += 8;

        if (def_offset > dict_raw_len || def_size > dict_raw_len - def_offset) {
            continue;
        }

        GString *article = g_string_new("");
        if (!append_stardict_article(article, dict_raw + def_offset, def_size, sametypesequence)) {
            g_string_assign(article, "");
            append_html_escaped_text(article, (const char *)dict_raw + def_offset, def_size);
        }

        long hw_off = ftell(cache_file);
        fwrite(hw_start, 1, hw_len, cache_file);
        fwrite("\n", 1, 1, cache_file);

        long def_off = ftell(cache_file);
        fwrite(article->str, 1, article->len, cache_file);
        fwrite("\n", 1, 1, cache_file);

        if (count == cap) {
            cap *= 2;
            entries = g_realloc(entries, cap * sizeof(TreeEntry));
        }

        entries[count].h_off = hw_off;
        entries[count].h_len = hw_len;
        entries[count].d_off = def_off;
        entries[count].d_len = article->len;
        count++;

        g_string_free(article, TRUE);
    }

    *entries_out = entries;
    *entry_count_out = count;
    return TRUE;
}

static DictMmap *open_cached_stardict(const char *cache_path, char *bookname, char *resource_dir) {
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

    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = fd;
    dict->data = data;
    dict->size = st.st_size;
    dict->name = bookname;
    dict->resource_dir = resource_dir;
    dict->index = splay_tree_new(dict->data, dict->size);

    uint64_t count = *(const uint64_t *)dict->data;
    size_t index_size = (size_t)count * sizeof(TreeEntry);
    if (count > 0 && index_size + 8 <= dict->size) {
        size_t index_off = dict->size - index_size;
        TreeEntry *tree_entries = (TreeEntry *)(dict->data + index_off);
        /* Validate entries before use to avoid reading invalid offsets */
        size_t data_region_end = dict->size - index_size;
        gboolean valid_index = TRUE;
        for (uint64_t i = 0; i < count; i++) {
            int64_t h_off = tree_entries[i].h_off;
            uint64_t h_len = tree_entries[i].h_len;
            int64_t d_off = tree_entries[i].d_off;
            uint64_t d_len = tree_entries[i].d_len;
            if (h_off < 8 || (uint64_t)h_off >= data_region_end) { valid_index = FALSE; break; }
            if (d_off < 8 || (uint64_t)d_off >= data_region_end) { valid_index = FALSE; break; }
            if (h_len == 0 || d_len == 0) { valid_index = FALSE; break; }
            if ((uint64_t)h_off + h_len > data_region_end) { valid_index = FALSE; break; }
            if ((uint64_t)d_off + d_len > data_region_end) { valid_index = FALSE; break; }
        }
        if (valid_index) {
            insert_balanced(dict->index, tree_entries, 0, (int)count - 1);
        } else {
            fprintf(stderr, "[IFO] Cache index validation failed for %s — rebuilding index.\n", cache_path);
        }
    }

    return dict;
}

DictMmap* parse_stardict(const char *ifo_path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    fprintf(stderr, "[IFO] Loading StarDict: %s\n", ifo_path);

    uint32_t wordcount = 0, idxfilesize = 0;
    char sametypesequence[64] = {0};
    char *bookname = NULL;

    if (parse_ifo_metadata(ifo_path, &wordcount, &idxfilesize,
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

    ensure_cache_directory();
    char *cache_path = get_cached_dict_path(ifo_path);
    const char *sources[] = { ifo_path, idx_path, dict_path };

    if (is_cache_valid_for_sources(cache_path, sources, G_N_ELEMENTS(sources))) {
        DictMmap *cached = open_cached_stardict(cache_path, bookname, resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        return cached;
    }

    unsigned char *dict_raw = NULL;
    size_t dict_raw_len = 0;
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

    if (!load_file_bytes_auto(dict_path, &dict_raw, &dict_raw_len) ||
        !load_file_bytes_auto(idx_path, &idx_data, &idx_size)) {
        fprintf(stderr, "[IFO] Failed reading .idx/.dict payloads\n");
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        g_free(dict_raw);
        g_free(idx_data);
        return NULL;
    }

    FILE *cache_file = fopen(cache_path, "wb");
    if (!cache_file) {
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        g_free(idx_path);
        g_free(dict_path);
        g_free(dict_raw);
        g_free(idx_data);
        return NULL;
    }

    uint64_t zero_count = 0;
    fwrite(&zero_count, 8, 1, cache_file);

    TreeEntry *entries = NULL;
    size_t entry_count = 0;
    gboolean built = build_stardict_cache(cache_file, idx_data, idx_size, dict_raw, dict_raw_len,
                                          sametypesequence, &entries, &entry_count);

    if (built) {
        fwrite(entries, sizeof(TreeEntry), entry_count, cache_file);
        fseek(cache_file, 0, SEEK_SET);
        uint64_t final_count = (uint64_t)entry_count;
        fwrite(&final_count, 8, 1, cache_file);
    }

    fclose(cache_file);
    sync_cache_mtime(cache_path, sources, G_N_ELEMENTS(sources));

    g_free(entries);
    g_free(dict_raw);
    g_free(idx_data);
    g_free(idx_path);
    g_free(dict_path);

    if (!built) {
        g_free(bookname);
        g_free(resource_dir);
        g_free(cache_path);
        return NULL;
    }

    DictMmap *dict = open_cached_stardict(cache_path, bookname, resource_dir);
    g_free(cache_path);
    return dict;
}
