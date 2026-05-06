#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ctype.h>
#include "dict-cache.h"
#include "dict-cache-builder.h"
#include "dictzip.h"
#include "settings.h"

/* Dictd support for local .index and .dict(.dz) files.
 * .index format: headword\toffset\tsize[\toriginal_headword]
 * Offset and size are in a custom base64-based encoding.
 */

static uint64_t decode_dictd_base64(const char *s) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint64_t number = 0;
    for (const char *p = s; *p; p++) {
        const char *d = strchr(digits, *p);
        if (!d) return 0;
        number = number * 64 + (uint64_t)(d - digits);
    }
    return number;
}

typedef struct {
    char *headword;
    uint64_t offset;
    uint64_t size;
} DictdIndexEntry;

static gint compare_index_entries(gconstpointer a, gconstpointer b) {
    const DictdIndexEntry *ea = (const DictdIndexEntry *)a;
    const DictdIndexEntry *eb = (const DictdIndexEntry *)b;
    if (ea->offset < eb->offset) return -1;
    if (ea->offset > eb->offset) return 1;
    return 0;
}

static char* get_dict_path(const char *index_path) {
    size_t len = strlen(index_path);
    if (len < 6) return NULL;
    char *base = g_strndup(index_path, len - 6);
    char *dict_dz = g_strconcat(base, ".dict.dz", NULL);
    if (g_file_test(dict_dz, G_FILE_TEST_EXISTS)) {
        g_free(base);
        return dict_dz;
    }
    g_free(dict_dz);
    char *dict = g_strconcat(base, ".dict", NULL);
    if (g_file_test(dict, G_FILE_TEST_EXISTS)) {
        g_free(base);
        return dict;
    }
    g_free(dict);
    g_free(base);
    return NULL;
}

DictMmap* parse_dictd_file(const char *index_path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    fprintf(stderr, "[DICTD] Loading Dictd: %s\n", index_path);

    char *dict_path = get_dict_path(index_path);
    if (!dict_path) {
        fprintf(stderr, "[DICTD] Missing .dict file for %s\n", index_path);
        return NULL;
    }

    dict_cache_ensure_dir();
    char *cache_path = dict_cache_path_for(index_path);
    struct stat st_idx, st_dict, st_cache;
    if (stat(index_path, &st_idx) == 0 && stat(dict_path, &st_dict) == 0 && stat(cache_path, &st_cache) == 0) {
        if (st_cache.st_mtime >= st_idx.st_mtime && st_cache.st_mtime >= st_dict.st_mtime) {
            /* Cache is valid */
            int fd = open(cache_path, O_RDONLY);
            if (fd >= 0) {
                struct stat st;
                fstat(fd, &st);
                const char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (data != MAP_FAILED) {
                    DictMmap *dict = g_new0(DictMmap, 1);
                    dict->fd = fd;
                    dict->data = data;
                    dict->size = st.st_size;
                    dict->index = flat_index_open(dict->data, dict->size);
                    if (dict->index) {
                        fprintf(stderr, "[DICTD] Loaded cache for %s (%zu entries)\n", index_path, dict->index->count);
                    }
                    if (dict_cache_is_compressed(dict->data, dict->size)) {
                        dict->is_compressed = TRUE;
                        dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, (const DictCacheHeader*)dict->data);
                    }
                    
                    /* Try to read dictionary name from cache if available */
                    /* (Normally the cache builder should handle this, but let's be safe) */
                    dict->name = g_path_get_basename(index_path);
                    char *dot = strrchr(dict->name, '.');
                    if (dot) *dot = '\0';

                    g_free(dict_path);
                    g_free(cache_path);
                    return dict;
                }
                close(fd);
            }
        }
    }

    /* Build cache */
    FILE *fidx = fopen(index_path, "r");
    if (!fidx) {
        g_free(dict_path);
        g_free(cache_path);
        return NULL;
    }

    DictZip *dz = NULL;
    FILE *fdict = NULL;
    if (g_str_has_suffix(dict_path, ".dz")) {
        dz = dictzip_open(dict_path);
    } else {
        fdict = fopen(dict_path, "rb");
    }

    if (!dz && !fdict) {
        fclose(fidx);
        g_free(dict_path);
        g_free(cache_path);
        return NULL;
    }

    /* Estimate entry count from index file size (approx 30 bytes per entry) */
    uint64_t est_count = (uint64_t)st_idx.st_size / 30;
    DictCacheBuilder *builder = dict_cache_builder_new(cache_path, est_count);
    if (!builder) {
        if (dz) dictzip_close(dz);
        if (fdict) fclose(fdict);
        fclose(fidx);
        g_free(dict_path);
        g_free(cache_path);
        return NULL;
    }


    GArray *index_entries = g_array_new(FALSE, FALSE, sizeof(DictdIndexEntry));
    char line[16384];
    char *bookname = NULL;
    while (fgets(line, sizeof(line), fidx)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';

        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char *headword = line;
        char *offset_s = tab1 + 1;
        char *tab2 = strchr(offset_s, '\t');
        if (!tab2) continue;
        *tab2 = '\0';
        char *size_s = tab2 + 1;
        char *tab3 = strchr(size_s, '\t');
        if (tab3) *tab3 = '\0';

        uint64_t offset = decode_dictd_base64(offset_s);
        uint64_t size = decode_dictd_base64(size_s);
        if (size == 0) continue;

        if (strcmp(headword, "00-database-short") == 0 || strcmp(headword, "00databaseshort") == 0) {
            size_t raw_len = 0;
            unsigned char *raw_def = dictzip_read(dz, offset, (uint32_t)size, &raw_len);
            if (raw_def) {
                char *p = (char*)raw_def;
                while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
                char *end = p;
                while (*end && *end != '\n' && *end != '\r') end++;
                bookname = g_strndup(p, (size_t)(end - p));
                free(raw_def);
            }
        }

        DictdIndexEntry ie = { g_strdup(headword), offset, size };
        g_array_append_val(index_entries, ie);
    }
    fclose(fidx);

    /* Sort by offset for sequential disk access and cache hits */
    g_array_sort(index_entries, (GCompareFunc)compare_index_entries);

    fprintf(stderr, "[DICTD] Processing %u entries sequentially...\n", index_entries->len);

    size_t count = 0;
    size_t cap = 4096;
    TreeEntry *entries = g_malloc(cap * sizeof(TreeEntry));

    for (guint i = 0; i < index_entries->len; i++) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        
        DictdIndexEntry *ie = &g_array_index(index_entries, DictdIndexEntry, i);
        size_t raw_len = 0;
        unsigned char *raw_def = dictzip_read(dz, ie->offset, (uint32_t)ie->size, &raw_len);
        if (!raw_def) continue;

        uint64_t h_off, d_off;
        dict_cache_builder_add_headword(builder, ie->headword, strlen(ie->headword), &h_off);
        dict_cache_builder_add_definition(builder, (const char*)raw_def, raw_len, &d_off);

        if (count >= cap) {
            cap *= 2;
            entries = g_realloc(entries, cap * sizeof(TreeEntry));
        }
        entries[count].h_off = h_off;
        entries[count].h_len = (uint32_t)strlen(ie->headword);
        entries[count].d_off = d_off;
        entries[count].d_len = (uint32_t)raw_len;
        count++;

        free(raw_def);
    }

    /* Cleanup temporary index entries */
    for (guint i = 0; i < index_entries->len; i++) {
        g_free(g_array_index(index_entries, DictdIndexEntry, i).headword);
    }
    g_array_free(index_entries, TRUE);

    if (count > 0) {
        dict_cache_builder_flush(builder);
        
        /* Sort entries by headword */
        FILE *rf = fopen(cache_path, "rb");
        if (rf) {
            struct stat sort_st;
            if (fstat(fileno(rf), &sort_st) == 0 && sort_st.st_size > 0) {
                char *cache_data = malloc((size_t)sort_st.st_size);
                if (fread(cache_data, 1, (size_t)sort_st.st_size, rf) == (size_t)sort_st.st_size) {
                    flat_index_sort_entries(entries, count, cache_data, (size_t)sort_st.st_size);
                }
                free(cache_data);
            }
            fclose(rf);
        }

        dict_cache_builder_finalize(builder, entries, count);
        fprintf(stderr, "[DICTD] Finalized cache with %zu entries\n", count);
    }

    dict_cache_builder_free(builder);
    g_free(entries);
    if (dz) dictzip_close(dz);
    if (fdict) fclose(fdict);

    /* Open the newly built cache */
    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) {
        g_free(dict_path);
        g_free(cache_path);
        g_free(bookname);
        return NULL;
    }
    struct stat final_st;
    fstat(fd, &final_st);
    const char *data = mmap(NULL, final_st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        g_free(dict_path);
        g_free(cache_path);
        g_free(bookname);
        return NULL;
    }

    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = fd;
    dict->data = data;
    dict->size = final_st.st_size;
    dict->index = flat_index_open(dict->data, dict->size);
    if (dict->index) {
        fprintf(stderr, "[DICTD] Built cache for %s (%zu entries)\n", index_path, dict->index->count);
    }
    if (dict_cache_is_compressed(dict->data, dict->size)) {
        dict->is_compressed = TRUE;
        dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, (const DictCacheHeader*)dict->data);
    }
    dict->name = bookname ? bookname : g_path_get_basename(index_path);
    if (!bookname) {
        char *dot = strrchr(dict->name, '.');
        if (dot) *dot = '\0';
    }

    /* Sync mtime */
    const char *sources[] = { index_path, dict_path };
    dict_cache_sync_mtime(cache_path, sources, 2);

    g_free(dict_path);
    g_free(cache_path);
    return dict;
}
