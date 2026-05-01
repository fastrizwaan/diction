#pragma once

#include <glib.h>

/* Shared cache directory helpers — used by all dictionary format parsers. */

/* Get the base user cache directory (cached, do not free). */
const char* dict_cache_base_dir(void);

/* Build path: $XDG_CACHE_HOME/diction/dicts  (caller must g_free). */
char* dict_cache_dir_path(void);

/* Build cache path for a given source file path using SHA1 hash (caller must g_free). */
char* dict_cache_path_for(const char *original_path);

/* TRUE if cache file exists and is at least as new as the source. */
gboolean dict_cache_is_valid(const char *cache_path, const char *original_path);

/* Ensure the diction/dicts cache directory exists. Returns TRUE on success. */
gboolean dict_cache_ensure_dir(void);

/* Ensure the target parent directory exists and there is reasonable free
 * space before writing bytes_needed to it. Returns TRUE when writes should
 * proceed. */
gboolean dict_cache_prepare_target_path(const char *target_path, guint64 bytes_needed);

/* Set the mtime of cache_path to match the newest source. */
void dict_cache_sync_mtime(const char *cache_path, const char **sources, int n_sources);
