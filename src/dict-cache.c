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
    gboolean valid = (cache_st.st_mtime >= orig_st.st_mtime);
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
