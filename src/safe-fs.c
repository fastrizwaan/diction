#include "safe-fs.h"
#undef unlink
#undef g_unlink
#undef remove
#undef fopen
#undef open

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

gboolean safe_fs_is_allowed(const char *path) {
    if (!path) return FALSE;
    char *abs_path = g_canonicalize_filename(path, NULL);
    
    char *cache = g_build_filename(g_get_user_cache_dir(), "diction", NULL);
    char *config = g_build_filename(g_get_user_config_dir(), "diction", NULL);
    char *data = g_build_filename(g_get_user_data_dir(), "diction", NULL);
    const char *tmp = g_get_tmp_dir();
    
    gboolean allowed = FALSE;
    if (g_str_has_prefix(abs_path, cache) ||
        g_str_has_prefix(abs_path, config) ||
        g_str_has_prefix(abs_path, data) ||
        g_str_has_prefix(abs_path, tmp) ||
        g_str_has_prefix(abs_path, "/tmp/")) {
        allowed = TRUE;
    }
    
    g_free(cache); g_free(config); g_free(data);
    
    if (!allowed) {
        g_printerr("[SECURITY] BLOCKED write/delete operation on outside path: %s\n", abs_path);
    }
    g_free(abs_path);
    return allowed;
}

int safe_unlink(const char *path) {
    if (!safe_fs_is_allowed(path)) {
        errno = EPERM;
        return -1;
    }
    return unlink(path);
}

int safe_g_unlink(const char *path) {
    if (!safe_fs_is_allowed(path)) {
        errno = EPERM;
        return -1;
    }
    return g_unlink(path);
}

int safe_remove(const char *path) {
    if (!safe_fs_is_allowed(path)) {
        errno = EPERM;
        return -1;
    }
    return remove(path);
}

FILE* safe_fopen(const char *path, const char *mode) {
    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
        if (!safe_fs_is_allowed(path)) {
            errno = EPERM;
            return NULL;
        }
    }
    return fopen(path, mode);
}

int safe_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    
    if ((flags & O_WRONLY) || (flags & O_RDWR) || (flags & O_CREAT) || (flags & O_TRUNC)) {
        if (!safe_fs_is_allowed(path)) {
            errno = EPERM;
            return -1;
        }
    }
    if (flags & O_CREAT) {
        return open(path, flags, mode);
    }
    return open(path, flags);
}
