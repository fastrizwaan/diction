#include "dict-cache.h"
#include <stdio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DICT_CACHE_WRITE_HEADROOM_MIN_BYTES (8ULL * 1024ULL * 1024ULL)
#define DICT_CACHE_WRITE_HEADROOM_MAX_BYTES (64ULL * 1024ULL * 1024ULL)
#define DICT_CACHE_FAILURE_SUFFIX ".fail"

const char* dict_cache_base_dir(void) {
    static const char *cache_dir = NULL;
    if (!cache_dir) cache_dir = g_get_user_cache_dir();
    return cache_dir;
}

char* dict_cache_dir_path(void) {
    const char *base = dict_cache_base_dir();
    return g_build_filename(base, "diction", "dicts", NULL);
}

static char* canonicalize_and_strip_path(const char *path) {
    if (!path) return g_strdup("");
    
    char *expanded = NULL;
    if (path[0] == '~') {
        char *expanded_tilde = g_build_filename(g_get_home_dir(), path + 1, NULL);
        expanded = g_canonicalize_filename(expanded_tilde, NULL);
        g_free(expanded_tilde);
    } else {
        expanded = g_canonicalize_filename(path, NULL);
    }
    
    if (!expanded) return g_strdup("");
    
    char *p = expanded;
    size_t len = strlen(p);
    
    /* Strip compressed/double extensions first */
    if (len > 7 && g_ascii_strcasecmp(p + len - 7, ".dsl.dz") == 0) {
        p[len - 7] = '\0';
    } else if (len > 8 && g_ascii_strcasecmp(p + len - 8, ".dict.dz") == 0) {
        p[len - 8] = '\0';
    } else if (len > 8 && g_ascii_strcasecmp(p + len - 8, ".xdxf.dz") == 0) {
        p[len - 8] = '\0';
    } else if (len > 8 && g_ascii_strcasecmp(p + len - 8, ".idx.gz") == 0) {
        p[len - 8] = '\0';
    }
    
    /* Strip single extensions */
    len = strlen(p);
    if (len > 4) {
        if (g_ascii_strcasecmp(p + len - 4, ".dsl") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".mdx") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".ifo") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".idx") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".bgl") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".xdxf") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".slob") == 0) {
            p[len - 4] = '\0';
        }
    }
    
    return p;
}

char* dict_cache_path_for(const char *original_path) {
    char *canon_strip = canonicalize_and_strip_path(original_path);
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, canon_strip, -1);
    g_free(canon_strip);
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

static char *dict_cache_failure_path(const char *cache_path) {
    return cache_path ? g_strconcat(cache_path, DICT_CACHE_FAILURE_SUFFIX, NULL) : NULL;
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

gboolean dict_cache_failure_is_current(const char *cache_path, const char *original_path) {
    char *failure_path = dict_cache_failure_path(cache_path);
    if (!failure_path) {
        return FALSE;
    }

    struct stat failure_st, orig_st;
    gboolean current = FALSE;
    if (stat(failure_path, &failure_st) == 0 &&
        stat(original_path, &orig_st) == 0 &&
        failure_st.st_size > 0 &&
        failure_st.st_mtime >= orig_st.st_mtime) {
        current = TRUE;
    }

    g_free(failure_path);
    return current;
}

void dict_cache_mark_failure(const char *cache_path, const char **sources, int n_sources) {
    char *failure_path = dict_cache_failure_path(cache_path);
    if (!failure_path) {
        return;
    }

    if (!dict_cache_prepare_target_path(failure_path, 1)) {
        g_free(failure_path);
        return;
    }

    FILE *fp = fopen(failure_path, "wb");
    if (fp) {
        fputc('\n', fp);
        fclose(fp);
        dict_cache_sync_mtime(failure_path, sources, n_sources);
    }

    g_free(failure_path);
}

void dict_cache_clear_failure(const char *cache_path) {
    char *failure_path = dict_cache_failure_path(cache_path);
    if (!failure_path) {
        return;
    }

    if (g_unlink(failure_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "[CACHE] Failed to remove failure marker %s: %s\n",
                failure_path, g_strerror(errno));
    }
    g_free(failure_path);
}

static gboolean dict_cache_name_has_hex_prefix(const char *name) {
    if (!name || strlen(name) < 40) {
        return FALSE;
    }

    for (int j = 0; j < 40; j++) {
        char c = name[j];
        if (!g_ascii_isxdigit(c)) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean dict_cache_name_is_managed_dict_file(const char *name) {
    if (!dict_cache_name_has_hex_prefix(name)) {
        return FALSE;
    }
    return name[40] == '\0' || strcmp(name + 40, DICT_CACHE_FAILURE_SUFFIX) == 0;
}

void dict_cache_garbage_collect(const GPtrArray *active_paths) {
    if (!active_paths) return;

    /* Build a set of all active cache hashes.  Dictionary cache files use the
     * same canonicalized/extension-stripped key as dict_cache_path_for(), while
     * FTS databases are keyed by the raw dictionary path.  Keep both so cleanup
     * never removes a freshly built cache for an active dictionary. */
    GHashTable *active_hashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < active_paths->len; i++) {
        const char *path = g_ptr_array_index(active_paths, i);
        if (path) {
            char *canon_strip = canonicalize_and_strip_path(path);
            char *cache_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, canon_strip, -1);
            char *fts_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);

            g_hash_table_add(active_hashes, cache_hash);
            g_hash_table_add(active_hashes, fts_hash);
            g_free(canon_strip);
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
            /* We only manage cache files that are SHA1 hashes, plus matching
             * tiny failure markers named <sha1>.fail. */
            if (dict_cache_name_is_managed_dict_file(name)) {
                char hash_prefix[41];
                strncpy(hash_prefix, name, 40);
                hash_prefix[40] = '\0';

                if (!g_hash_table_contains(active_hashes, hash_prefix)) {
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
