/* dict-ifo.c — StarDict .ifo + .idx + .dict(.dz) parser
 *
 * StarDict uses three files:
 *   .ifo  — plain-text metadata (wordcount, idxfilesize, sametypesequence)
 *   .idx  — binary index: for each entry: null-terminated headword +
 *            4-byte BE offset + 4-byte BE size pointing into .dict
 *   .dict — concatenated definitions (may be .dict.dz = dictzip)
 *
 * We parse .ifo for metadata, then .idx to build the SplayTree,
 * and mmap .dict data.
 *
 * Reference: goldendict-ng/src/dict/stardict.cc
 */

#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <utime.h>
#include <fcntl.h>
#include <glib.h>

/* Cache helpers */
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

static gboolean is_cache_valid(const char *cache_path, const char *original_path) {
    struct stat cache_st, orig_st;
    if (stat(cache_path, &cache_st) != 0 || stat(original_path, &orig_st) != 0)
        return FALSE;
    return cache_st.st_mtime >= orig_st.st_mtime;
}

static gboolean ensure_cache_directory(void) {
    char *cache_dir = get_cache_dir_path();
    int ret = g_mkdir_with_parents(cache_dir, 0755);
    g_free(cache_dir);
    return ret == 0;
}

static uint32_t read_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Parse a .ifo file and extract wordcount and idxfilesize */
static int parse_ifo_metadata(const char *ifo_path,
                               uint32_t *wordcount,
                               uint32_t *idxfilesize,
                               char *sametypesequence,
                               size_t sts_len) {
    FILE *f = fopen(ifo_path, "r");
    if (!f) return -1;

    char line[1024];
    *wordcount = 0;
    *idxfilesize = 0;
    sametypesequence[0] = '\0';

    /* First line should be "StarDict's dict ifo file" */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    if (strncmp(line, "StarDict's dict ifo file", 24) != 0) {
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        /* Remove trailing newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        if (strncmp(line, "wordcount=", 10) == 0) {
            *wordcount = (uint32_t)atol(line + 10);
        } else if (strncmp(line, "idxfilesize=", 12) == 0) {
            *idxfilesize = (uint32_t)atol(line + 12);
        } else if (strncmp(line, "sametypesequence=", 17) == 0) {
            strncpy(sametypesequence, line + 17, sts_len - 1);
            sametypesequence[sts_len - 1] = '\0';
        }
    }
    fclose(f);
    return 0;
}

DictMmap* parse_stardict(const char *ifo_path) {
    printf("[IFO] Loading StarDict: %s\n", ifo_path);

    uint32_t wordcount = 0, idxfilesize = 0;
    char sametypesequence[32] = {0};

    if (parse_ifo_metadata(ifo_path, &wordcount, &idxfilesize,
                           sametypesequence, sizeof(sametypesequence)) != 0) {
        fprintf(stderr, "[IFO] Failed to parse .ifo: %s\n", ifo_path);
        return NULL;
    }

    printf("[IFO] wordcount=%u, idxfilesize=%u, sametypesequence='%s'\n",
           wordcount, idxfilesize, sametypesequence);

    /* Derive sibling file paths from .ifo path */
    size_t base_len = strlen(ifo_path) - 4; /* strip ".ifo" */
    char *idx_path = malloc(base_len + 8);
    char *dict_path = malloc(base_len + 10);

    memcpy(idx_path, ifo_path, base_len);
    memcpy(dict_path, ifo_path, base_len);

    /* Try .idx.gz first, then .idx */
    strcpy(idx_path + base_len, ".idx");
    struct stat st;
    if (stat(idx_path, &st) != 0) {
        /* No plain .idx, try .idx.gz — but for simplicity, just fail */
        fprintf(stderr, "[IFO] No .idx file found\n");
        free(idx_path); free(dict_path);
        return NULL;
    }

    /* Try .dict.dz first, then .dict */
    strcpy(dict_path + base_len, ".dict.dz");
    int dict_is_dz = 1;
    if (stat(dict_path, &st) != 0) {
        strcpy(dict_path + base_len, ".dict");
        dict_is_dz = 0;
        if (stat(dict_path, &st) != 0) {
            fprintf(stderr, "[IFO] No .dict or .dict.dz found\n");
            free(idx_path); free(dict_path);
            return NULL;
        }
    }

    printf("[IFO] idx: %s, dict: %s (dz=%d)\n", idx_path, dict_path, dict_is_dz);

    // Ensure cache directory exists
    ensure_cache_directory();

    // Get cache path for this dictionary (based on .ifo path)
    char *cache_path = get_cached_dict_path(ifo_path);
    gboolean cache_exists = (access(cache_path, F_OK) == 0);
    gboolean cache_valid = cache_exists && is_cache_valid(cache_path, ifo_path);

    int cache_fd = -1;
    const char *dict_data = NULL;
    size_t dict_size = 0;

    if (cache_valid) {
        // Use cached version directly
        printf("[IFO] Loading from cache: %s\n", cache_path);
        cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd < 0) {
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }

        struct stat st;
        if (fstat(cache_fd, &st) < 0 || st.st_size == 0) {
            close(cache_fd);
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }
        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
        if (dict_data == MAP_FAILED) {
            close(cache_fd);
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }
    } else {
        // Need to extract and convert
        printf("[IFO] Building cache from source files\n");

        // Open cache file for writing
        FILE *cache_file = fopen(cache_path, "wb");
        if (!cache_file) {
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }

        gzFile gz = gzopen(dict_path, "rb");
        if (!gz) {
            fclose(cache_file);
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }

        // Read entire dict data into heap
        size_t dict_data_cap = 1024 * 1024;
        unsigned char *dict_raw = malloc(dict_data_cap);
        size_t dict_raw_len = 0;
        unsigned char buf[65536];
        int n;
        while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
            if (dict_raw_len + n > dict_data_cap) {
                dict_data_cap *= 2;
                dict_raw = realloc(dict_raw, dict_data_cap);
            }
            memcpy(dict_raw + dict_raw_len, buf, n);
            dict_raw_len += n;
        }
        gzclose(gz);

        // Read .idx file
        FILE *idx_file = fopen(idx_path, "rb");
        if (!idx_file) {
            free(dict_raw);
            fclose(cache_file);
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }

        fseek(idx_file, 0, SEEK_END);
        long idx_size = ftell(idx_file);
        fseek(idx_file, 0, SEEK_SET);

        unsigned char *idx_data = malloc(idx_size);
        if (fread(idx_data, 1, idx_size, idx_file) != (size_t)idx_size) {
            free(idx_data);
            free(dict_raw);
            fclose(cache_file);
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }
        fclose(idx_file);

        // Write interleaved "headword\ndef\n" into cache file
        const unsigned char *ip = idx_data;
        const unsigned char *ie = idx_data + idx_size;

        while (ip < ie) {
            const unsigned char *hw_start = ip;
            while (ip < ie && *ip != '\0') ip++;
            size_t hw_len = ip - hw_start;
            if (ip >= ie) break;
            ip++; /* skip null */

            if (ip + 8 > ie) break;
            uint32_t def_offset = read_u32be(ip); ip += 4;
            uint32_t def_size = read_u32be(ip); ip += 4;

            /* Write headword line */
            fwrite(hw_start, 1, hw_len, cache_file);
            fwrite("\n", 1, 1, cache_file);

            /* Write definition */
            if (def_offset + def_size <= dict_raw_len) {
                fwrite(dict_raw + def_offset, 1, def_size, cache_file);
            }
            fwrite("\n", 1, 1, cache_file);
        }

        free(dict_raw);
        free(idx_data);
        fflush(cache_file);
        fclose(cache_file);

        // Update cache file mtime to match source
        struct stat src_st;
        if (stat(ifo_path, &src_st) == 0) {
            struct utimbuf times;
            times.actime = src_st.st_mtime;
            times.modtime = src_st.st_mtime;
            utime(cache_path, &times);
        }

        // Now open the cached file
        cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd < 0) {
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }

        struct stat st;
        if (fstat(cache_fd, &st) < 0 || st.st_size == 0) {
            close(cache_fd);
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }
        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
        if (dict_data == MAP_FAILED) {
            close(cache_fd);
            g_free(cache_path);
            free(idx_path); free(dict_path);
            return NULL;
        }
    }

    g_free(cache_path);

    /* Build DictMmap */
    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = cache_fd;
    dict->tmp_file = NULL;
    dict->data = dict_data;
    dict->size = dict_size;
    dict->index = splay_tree_new(dict->data, dict->size);

    /* Read .idx file and parse entries for indexing */
    FILE *idx_file = fopen(idx_path, "rb");
    if (!idx_file) {
        fprintf(stderr, "[IFO] Failed to open .idx: %s\n", idx_path);
        dict_mmap_close(dict);
        free(idx_path); free(dict_path);
        return NULL;
    }

    /* Read entire .idx into memory (it's typically small) */
    fseek(idx_file, 0, SEEK_END);
    long idx_size = ftell(idx_file);
    fseek(idx_file, 0, SEEK_SET);

    unsigned char *idx_data = malloc(idx_size);
    if (fread(idx_data, 1, idx_size, idx_file) != (size_t)idx_size) {
        free(idx_data);
        fclose(idx_file);
        dict_mmap_close(dict);
        free(idx_path); free(dict_path);
        return NULL;
    }
    fclose(idx_file);

    /* Index the cached file into SplayTree */
    const char *dp = dict->data;
    const char *de = dp + dict->size;
    int indexed = 0;

    while (dp < de) {
        /* Headword line */
        const char *hw_start = dp;
        while (dp < de && *dp != '\n') dp++;
        size_t hw_len = dp - hw_start;
        if (dp < de) dp++; /* skip \n */

        /* Definition */
        const char *def_start = dp;
        while (dp < de && *dp != '\n') dp++;
        size_t def_len = dp - def_start;
        if (dp < de) dp++; /* skip \n */

        if (hw_len > 0 && def_len > 0) {
            splay_tree_insert(dict->index,
                              hw_start - dict->data, hw_len,
                              def_start - dict->data, def_len);
            indexed++;
        }
    }

    free(idx_data);
    free(idx_path);
    free(dict_path);

    printf("[IFO] Indexed %d StarDict entries (cached)\n", indexed);
    return dict;
}
