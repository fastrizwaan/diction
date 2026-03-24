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

    /* Load and decompress .dict into a tmpfile for mmap */
    FILE *tmp = tmpfile();
    if (!tmp) {
        free(idx_path); free(dict_path);
        return NULL;
    }

    gzFile gz = gzopen(dict_path, "rb");
    if (!gz) {
        fclose(tmp);
        free(idx_path); free(dict_path);
        return NULL;
    }

    unsigned char buf[65536];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, tmp);
    }
    gzclose(gz);
    fflush(tmp);

    int tmp_fd = fileno(tmp);
    if (fstat(tmp_fd, &st) < 0 || st.st_size == 0) {
        fclose(tmp);
        free(idx_path); free(dict_path);
        return NULL;
    }

    void *dict_map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, tmp_fd, 0);
    if (dict_map == MAP_FAILED) {
        fclose(tmp);
        free(idx_path); free(dict_path);
        return NULL;
    }

    /* Build DictMmap */
    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = tmp_fd;
    dict->tmp_file = tmp;
    dict->data = (const char *)dict_map;
    dict->size = st.st_size;
    dict->index = splay_tree_new(dict->data, dict->size);

    /* Read .idx file and parse entries */
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

    /* Parse .idx: each entry is:
     *   null-terminated UTF-8 headword
     *   4-byte BE offset into .dict
     *   4-byte BE size of definition
     *
     * We can't store the headword offset from .idx (different buffer),
     * so we need to find the headword in the .dict data or do something
     * creative. The headword ISN'T stored in .dict — only definitions.
     *
     * For StarDict, sametypesequence often starts with 'm' (text) or 'h' (html).
     * The definition data IS in our mmap'd dict buffer.
     *
     * Problem: the headword string is in the .idx file, not in .dict.
     * Solution: We need to create a combined buffer that interleaves
     * headwords and definitions. Let's rebuild the tmpfile with
     * headword\n + definition for each entry.
     */

    /* Rewrite: build a new tmpfile with "headword\ndef_body" blocks */
    munmap((void *)dict->data, dict->size);
    fseek(tmp, 0, SEEK_SET);
    if (ftruncate(tmp_fd, 0) != 0) {
        perror("[IFO] ftruncate");
    }

    /* First pass: figure out all entries from .idx */
    const unsigned char *ip = idx_data;
    const unsigned char *ie = idx_data + idx_size;
    int entry_count = 0;

    /* We need to iterate .idx twice: once to rebuild, once to index.
     * Let's do it in one pass: read .dict data, build combined buffer. */

    /* Read .dict data from the gzip again or directly from our mapped copy.
     * We already have the data in the original mmap, but we unmapped it.
     * Re-open and read: */

    /* Actually, let's re-read the dict via gzopen */
    gz = gzopen(dict_path, "rb");
    if (!gz) {
        free(idx_data);
        fclose(tmp);
        free(dict); free(idx_path); free(dict_path);
        return NULL;
    }

    /* Read entire dict data into heap temporarily */
    size_t dict_data_cap = 1024 * 1024;
    unsigned char *dict_raw = malloc(dict_data_cap);
    size_t dict_raw_len = 0;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        if (dict_raw_len + n > dict_data_cap) {
            dict_data_cap *= 2;
            dict_raw = realloc(dict_raw, dict_data_cap);
        }
        memcpy(dict_raw + dict_raw_len, buf, n);
        dict_raw_len += n;
    }
    gzclose(gz);

    /* Now write interleaved "headword\ndef\n" into tmpfile */
    ip = idx_data;
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
        size_t hw_file_offset = ftell(tmp);
        fwrite(hw_start, 1, hw_len, tmp);
        fwrite("\n", 1, 1, tmp);

        /* Write definition */
        size_t def_file_offset = ftell(tmp);
        if (def_offset + def_size <= dict_raw_len) {
            fwrite(dict_raw + def_offset, 1, def_size, tmp);
        }
        fwrite("\n", 1, 1, tmp);

        entry_count++;
        (void)hw_file_offset;
        (void)def_file_offset;
    }

    free(dict_raw);
    fflush(tmp);

    /* Re-mmap the combined file */
    if (fstat(tmp_fd, &st) < 0 || st.st_size == 0) {
        free(idx_data); fclose(tmp);
        free(dict); free(idx_path); free(dict_path);
        return NULL;
    }

    dict_map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, tmp_fd, 0);
    if (dict_map == MAP_FAILED) {
        free(idx_data); fclose(tmp);
        free(dict); free(idx_path); free(dict_path);
        return NULL;
    }

    dict->data = (const char *)dict_map;
    dict->size = st.st_size;
    splay_tree_free(dict->index);
    dict->index = splay_tree_new(dict->data, dict->size);

    /* Second pass: index the combined file into SplayTree */
    const char *dp = dict->data;
    const char *de = dp + dict->size;
    int indexed = 0;

    while (dp < de) {
        /* Headword line */
        const char *hw_start = dp;
        while (dp < de && *dp != '\n') dp++;
        size_t hw_len = dp - hw_start;
        if (dp < de) dp++; /* skip \n */

        /* Definition: everything until next headword (non-indented)
         * Actually, in our format it's just one line/block followed by \n */
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

    printf("[IFO] Indexed %d StarDict entries\n", indexed);
    return dict;
}
