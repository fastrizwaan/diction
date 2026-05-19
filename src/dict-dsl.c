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
#include "settings.h"
#include "dict-chunked.h"
#include "dict-cache-builder.h"
#include "dictzip.h"
#include "dict-dsl-index.h"

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
        *out_reader = resource_reader_open_archive(zip_path, resource_dir);
    }

    g_free(zip_path);
    return resource_dir;
}



/* New signature accepts cancel flag and expected generation for cooperative cancellation. */
DictMmap* dict_mmap_open(const char *path, volatile gint *cancel_flag, gint expected) {
    (void)cancel_flag;
    (void)expected;
    if (!path) return NULL;
    size_t path_len = strlen(path);
    if (path_len > 4 && strcasecmp(path + path_len - 4, ".mdx") == 0) {
        fprintf(stderr, "MDX decompression mapping is currently in Phase 2 Development.\n");
        return NULL;
    }

    dict_cache_ensure_dir();

    // Get cache path for this dictionary
    char *cache_path = dict_cache_path_for(path);
    gboolean cache_exists = (access(cache_path, F_OK) == 0);
    gboolean cache_valid = cache_exists && dict_cache_is_valid(cache_path, path);
    if (!cache_valid && dict_cache_failure_is_current(cache_path, path)) {
        fprintf(stderr, "[DSL] Skipping cached index failure for %s\n", path);
        g_free(cache_path);
        return NULL;
    }

    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = -1;
    dict->tmp_file = NULL;
    dict->source_dir = g_path_get_dirname(path);
    dict->resource_dir = dsl_prepare_resource_dir(path, &dict->resource_reader);

    if (!cache_valid) {
        printf("[DSL] Building index-only cache for %s\n", path);
        char *tmp_cache = g_strdup_printf("%s.tmp", cache_path);
        
        if (!build_dsl_index_only_cache(path, tmp_cache)) {
            fprintf(stderr, "[DSL] Failed to build index cache for %s\n", path);
            const char *sources[] = { path };
            dict_cache_mark_failure(cache_path, sources, 1);
            unlink(tmp_cache);
            g_free(tmp_cache);
            g_free(cache_path);
            g_free(dict->source_dir);
            resource_reader_close(dict->resource_reader);
            g_free(dict);
            return NULL;
        }

        struct stat src_st;
        if (stat(path, &src_st) == 0) {
            struct utimbuf times = { .actime = src_st.st_mtime, .modtime = src_st.st_mtime };
            utime(tmp_cache, &times);
        }

        if (rename(tmp_cache, cache_path) != 0) {
            fprintf(stderr, "[DSL] Failed to rename temp cache to %s\n", cache_path);
            unlink(tmp_cache);
            g_free(tmp_cache);
            g_free(cache_path);
            g_free(dict->source_dir);
            resource_reader_close(dict->resource_reader);
            g_free(dict);
            return NULL;
        }
        const char *sources[] = { path };
        dict_cache_sync_mtime(cache_path, sources, 1);
        dict_cache_clear_failure(cache_path);
        g_free(tmp_cache);
    } else {
        printf("Loading Dictionary from cache: %s\n", cache_path);
    }

    dict->fd = open(cache_path, O_RDONLY);
    if (dict->fd < 0) {
        g_free(cache_path);
        g_free(dict->source_dir);
        resource_reader_close(dict->resource_reader);
        g_free(dict);
        return NULL;
    }

    struct stat st;
    fstat(dict->fd, &st);
    dict->size = st.st_size;

    void *map = mmap(NULL, dict->size, PROT_READ, MAP_SHARED, dict->fd, 0);
    if (map == MAP_FAILED) {
        close(dict->fd);
        g_free(cache_path);
        g_free(dict->source_dir);
        resource_reader_close(dict->resource_reader);
        g_free(dict);
        return NULL;
    }

    dict->data = (const char*)map;
    dict->index = flat_index_open(dict->data, dict->size);

    if (dict_cache_is_compressed(dict->data, dict->size)) {
        const DictCacheHeader *header = (const DictCacheHeader*)dict->data;
        dict->is_compressed = TRUE;
        if (header->chunk_count == 0) {
            /* Index-only source-backed cache */
            if (g_str_has_suffix(path, ".dz")) {
                dict->source_dz = dictzip_open(path);
            } else {
                dict->source_fd = open(path, O_RDONLY);
                if (dict->source_fd >= 0) {
                    struct stat s_st;
                    fstat(dict->source_fd, &s_st);
                    dict->source_size = s_st.st_size;
                    dict->source_mmap = mmap(NULL, dict->source_size, PROT_READ, MAP_SHARED, dict->source_fd, 0);
                }
            }
            dict->source_encoding = header->source_encoding;
        } else {
            /* Legacy fully-compressed chunked cache */
            dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, header);
        }
    }

    if (!dict->name) {
        char *base = g_path_get_basename(path);
        dict->name = g_strdup(base);
        g_free(base);
    }

    g_free(cache_path);
    return dict;
}
