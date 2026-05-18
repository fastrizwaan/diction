#include "dict-cache.h"
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DICT_CACHE_WRITE_HEADROOM_MIN_BYTES (8ULL * 1024ULL * 1024ULL)
#define DICT_CACHE_WRITE_HEADROOM_MAX_BYTES (64ULL * 1024ULL * 1024ULL)

const char* dict_cache_base_dir(void) {
    static const char *cache_dir = NULL;
    if (!cache_dir) cache_dir = g_get_user_cache_dir();
    return cache_dir;
}

char* dict_cache_dir_path(void) {
    const char *base = dict_cache_base_dir();
    return g_build_filename(base, "diction", "dicts", NULL);
}

char* dict_cache_path_for(const char *original_path) {
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, original_path, -1);
    const char *base = dict_cache_base_dir();
    char *path = g_build_filename(base, "diction", "dicts", hash, NULL);
    g_free(hash);
    return path;
}

gboolean dict_cache_is_valid(const char *cache_path, const char *original_path) {
    struct stat cache_st, orig_st;
    if (stat(cache_path, &cache_st) != 0) {
        // fprintf(stderr, "[CACHE] Missing cache: %s\n", cache_path);
        return FALSE;
    }
    if (stat(original_path, &orig_st) != 0) {
        return FALSE;
    }
    gboolean valid = (cache_st.st_size > 0 && cache_st.st_mtime >= orig_st.st_mtime);
    if (!valid) {
        fprintf(stderr, "[CACHE] MISS (outdated): %s (cache=%ld, orig=%ld)\n", original_path, (long)cache_st.st_mtime, (long)orig_st.st_mtime);
    } else {
        fprintf(stderr, "[CACHE] HIT: %s\n", original_path);
    }
    return valid;
}

gboolean dict_cache_ensure_dir(void) {
    char *dir = dict_cache_dir_path();
    int ret = g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    return ret == 0;
}

static guint64 dict_cache_required_free_bytes(guint64 bytes_needed) {
    guint64 headroom = bytes_needed / 4;
    if (headroom < DICT_CACHE_WRITE_HEADROOM_MIN_BYTES) {
        headroom = DICT_CACHE_WRITE_HEADROOM_MIN_BYTES;
    }
    if (headroom > DICT_CACHE_WRITE_HEADROOM_MAX_BYTES) {
        headroom = DICT_CACHE_WRITE_HEADROOM_MAX_BYTES;
    }

    if (G_MAXUINT64 - bytes_needed < headroom) {
        return G_MAXUINT64;
    }
    return bytes_needed + headroom;
}

gboolean dict_cache_prepare_target_path(const char *target_path, guint64 bytes_needed) {
    if (!target_path || !*target_path) {
        return FALSE;
    }

    char *dir = g_path_get_dirname(target_path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        fprintf(stderr, "[CACHE] Failed to create directory %s: %s\n",
                dir, g_strerror(errno));
        g_free(dir);
        return FALSE;
    }

    struct statvfs fs;
    if (statvfs(dir, &fs) != 0) {
        fprintf(stderr, "[CACHE] Unable to check free space for %s: %s\n",
                dir, g_strerror(errno));
        g_free(dir);
        return TRUE;
    }

    guint64 free_bytes = (guint64)fs.f_bavail * (guint64)fs.f_frsize;
    guint64 required_bytes = dict_cache_required_free_bytes(bytes_needed);
    if (free_bytes < required_bytes) {
        fprintf(stderr,
                "[CACHE] Not enough free space for %s (need about %" G_GUINT64_FORMAT
                " bytes, have %" G_GUINT64_FORMAT ")\n",
                target_path, required_bytes, free_bytes);
        g_free(dir);
        return FALSE;
    }

    g_free(dir);
    return TRUE;
}

void dict_cache_sync_mtime(const char *cache_path, const char **sources, int n_sources) {
    time_t newest = 0;
    for (int i = 0; i < n_sources; i++) {
        if (!sources[i]) continue;
        struct stat st;
        if (stat(sources[i], &st) == 0 && st.st_mtime > newest)
            newest = st.st_mtime;
    }
    if (newest > 0) {
        struct utimbuf times = { .actime = newest, .modtime = newest };
        utime(cache_path, &times);
    }
}

#include <glib/gstdio.h>

void dict_cache_garbage_collect(const GPtrArray *active_paths) {
    if (!active_paths) return;

    /* Build a set of all active cache hashes */
    GHashTable *active_hashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < active_paths->len; i++) {
        const char *path = g_ptr_array_index(active_paths, i);
        if (path) {
            char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
            g_hash_table_add(active_hashes, hash);
        }
    }

    /* 1. Clean dict cache directory */
    char *dir_path = dict_cache_dir_path();
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir) {
        const char *name = NULL;
        guint64 freed_bytes = 0;
        guint64 deleted_count = 0;
        
        while ((name = g_dir_read_name(dir)) != NULL) {
            /* We only manage cache files that are SHA1 hashes (40-char hex string) */
            if (strlen(name) == 40) {
                gboolean is_hex = TRUE;
                for (int j = 0; j < 40; j++) {
                    char c = name[j];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                        is_hex = FALSE;
                        break;
                    }
                }
                if (is_hex) {
                    if (!g_hash_table_contains(active_hashes, name)) {
                        char *file_path = g_build_filename(dir_path, name, NULL);
                        struct stat st;
                        if (stat(file_path, &st) == 0) {
                            freed_bytes += (guint64)st.st_size;
                        }
                        if (g_unlink(file_path) == 0) {
                            deleted_count++;
                        }
                        g_free(file_path);
                    }
                }
            }
        }
        g_dir_close(dir);
        if (deleted_count > 0) {
            fprintf(stderr, "[CACHE GC] Cleaned up %" G_GUINT64_FORMAT " orphaned dictionary cache files, freeing %" G_GUINT64_FORMAT " bytes.\n",
                    deleted_count, freed_bytes);
        }
    }
    g_free(dir_path);

    /* 2. Clean fts database directory */
    const char *base = dict_cache_base_dir();
    char *fts_dir = g_build_filename(base, "diction", "fts", NULL);
    GDir *fdir = g_dir_open(fts_dir, 0, NULL);
    if (fdir) {
        const char *name = NULL;
        guint64 freed_bytes = 0;
        guint64 deleted_count = 0;

        while ((name = g_dir_read_name(fdir)) != NULL) {
            /* We clean files starting with a 40-character SHA1 hash */
            if (strlen(name) >= 40) {
                gboolean is_hex = TRUE;
                for (int j = 0; j < 40; j++) {
                    char c = name[j];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                        is_hex = FALSE;
                        break;
                    }
                }
                if (is_hex) {
                    /* Get the 40-char hash prefix */
                    char hash_prefix[41];
                    strncpy(hash_prefix, name, 40);
                    hash_prefix[40] = '\0';

                    if (!g_hash_table_contains(active_hashes, hash_prefix)) {
                        char *file_path = g_build_filename(fts_dir, name, NULL);
                        struct stat st;
                        if (stat(file_path, &st) == 0) {
                            freed_bytes += (guint64)st.st_size;
                        }
                        if (g_unlink(file_path) == 0) {
                            deleted_count++;
                        }
                        g_free(file_path);
                    }
                }
            }
        }
        g_dir_close(fdir);
        if (deleted_count > 0) {
            fprintf(stderr, "[CACHE GC] Cleaned up %" G_GUINT64_FORMAT " orphaned FTS SQLite database files, freeing %" G_GUINT64_FORMAT " bytes.\n",
                    deleted_count, freed_bytes);
        }
    }
    g_free(fts_dir);
    
    g_hash_table_unref(active_hashes);
}
