#include "dict-cache.h"
#include <stdio.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>
#include <string.h>

const char* dict_cache_base_dir(void) {
    static const char *cache_dir = NULL;
    if (!cache_dir) cache_dir = g_get_user_cache_dir();
    return cache_dir;
}

char* dict_cache_dir_path(void) {
    const char *base = dict_cache_base_dir();
    return g_build_filename(base, "diction", "dicts-v7", NULL);
}

char* dict_cache_path_for(const char *original_path) {
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, original_path, -1);
    const char *base = dict_cache_base_dir();
    char *path = g_build_filename(base, "diction", "dicts-v7", hash, NULL);
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
