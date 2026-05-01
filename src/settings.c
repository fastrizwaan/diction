#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include "dict-mmap.h"
#include "dict-cache.h"

static GPtrArray *settings_collect_mdx_companion_paths(const char *mdx_path);
static gboolean ends_with_ci(const char *text, const char *suffix);


static char *settings_fallback_dictionary_name(const char *path) {
    char *basename = g_path_get_basename(path ? path : "");
    if (!basename) {
        return g_strdup(path ? path : "");
    }

    if (ends_with_ci(basename, ".dsl.dz")) {
        basename[strlen(basename) - 7] = '\0';
    } else {
        char *ext = strrchr(basename, '.');
        if (ext) {
            *ext = '\0';
        }
    }

    return basename;
}

char* settings_resolve_dictionary_name(const char *path) {
    if (!path || !*path) {
        return g_strdup("");
    }

    DictFormat fmt = dict_detect_format(path);
    DictMmap *dict = NULL;

    switch (fmt) {
        case DICT_FORMAT_DSL:
            dict = dict_mmap_open(path, NULL, 0);
            break;
        case DICT_FORMAT_STARDICT:
            dict = parse_stardict(path, NULL, 0);
            break;
        case DICT_FORMAT_MDX:
            dict = parse_mdx_file(path, NULL, 0);
            break;
        case DICT_FORMAT_BGL:
            dict = parse_bgl_file(path, NULL, 0);
            break;
        case DICT_FORMAT_SLOB:
            dict = parse_slob_file(path, NULL, 0);
            break;
        default:
            break;
    }

    if (dict && dict->name && *dict->name) {
        char *valid = g_utf8_make_valid(dict->name, -1);
        char *resolved = g_strdup(valid);
        g_free(valid);
        dict_mmap_close(dict);
        return resolved;
    }

    if (dict) {
        dict_mmap_close(dict);
    }

    return settings_fallback_dictionary_name(path);
}

static const char* get_config_dir(void) {
    static const char *config_dir = NULL;
    if (!config_dir) {
        config_dir = g_get_user_config_dir();
    }
    return config_dir;
}

static char* get_settings_file_path(void) {
    const char *base = get_config_dir();
    return g_build_filename(base, "diction", SETTINGS_FILE, NULL);
}

static void ensure_config_dir(void) {
    const char *base = get_config_dir();
    char *dir = g_build_filename(base, "diction", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
}

static gboolean ends_with_ci(const char *text, const char *suffix) {
    if (!text || !suffix) {
        return FALSE;
    }

    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (text_len < suffix_len) {
        return FALSE;
    }

    return g_ascii_strcasecmp(text + text_len - suffix_len, suffix) == 0;
}

static char *strip_suffix_dup(const char *text, const char *suffix) {
    if (!text) {
        return g_strdup("");
    }

    if (!suffix || !ends_with_ci(text, suffix)) {
        return g_strdup(text);
    }

    return g_strndup(text, strlen(text) - strlen(suffix));
}

static char *get_dictionary_data_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "diction", "dicts", NULL);
}

static DictConfig *settings_find_dictionary_by_id_locked(AppSettings *settings, const char *id) {
    if (!settings || !id || !settings->dictionaries) {
        return NULL;
    }

    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (cfg && g_strcmp0(cfg->id, id) == 0) {
            return cfg;
        }
    }

    return NULL;
}

static DictConfig *settings_find_dictionary_by_path_locked(AppSettings *settings, const char *path) {
    if (!settings || !path || !settings->dictionaries) {
        return NULL;
    }

    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (cfg && cfg->path && g_strcmp0(cfg->path, path) == 0) {
            return cfg;
        }
    }

    return NULL;
}

gboolean settings_path_is_in_directory_list(AppSettings *settings, const char *path) {
    if (!settings || !path || !*path) {
        return FALSE;
    }

    for (guint i = 0; i < settings->dictionary_dirs->len; i++) {
        const char *dir_path = g_ptr_array_index(settings->dictionary_dirs, i);
        if (path_is_inside_dir(path, dir_path)) {
            return TRUE;
        }
    }

    return FALSE;
}

gboolean path_is_inside_dir(const char *path, const char *dir_path) {
    if (!path || !dir_path) {
        return FALSE;
    }

    char *canon_path = g_canonicalize_filename(path, NULL);
    char *canon_dir = g_canonicalize_filename(dir_path, NULL);
    char *canon_dir_sep = g_str_has_suffix(canon_dir, G_DIR_SEPARATOR_S)
        ? g_strdup(canon_dir)
        : g_strconcat(canon_dir, G_DIR_SEPARATOR_S, NULL);

    gboolean inside = g_strcmp0(canon_path, canon_dir) == 0 ||
                      g_str_has_prefix(canon_path, canon_dir_sep);

    g_free(canon_path);
    g_free(canon_dir);
    g_free(canon_dir_sep);
    return inside;
}

static gboolean remove_path_recursive(const char *path) {
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        return TRUE;
    }

    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        return g_remove(path) == 0;
    }

    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) {
        return FALSE;
    }

    gboolean ok = TRUE;
    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *child = g_build_filename(path, name, NULL);
        if (!remove_path_recursive(child)) {
            ok = FALSE;
        }
        g_free(child);
    }

    g_dir_close(dir);
    if (g_rmdir(path) != 0) {
        ok = FALSE;
    }

    return ok;
}

typedef enum {
    CLEANUP_CACHE_ONLY = 0,
    CLEANUP_DELETE_PAYLOAD
} CleanupKind;

typedef struct {
    char *path;
    CleanupKind kind;
} CleanupTask;

static void cleanup_task_free(CleanupTask *task) {
    if (!task) {
        return;
    }

    g_free(task->path);
    g_free(task);
}

static GPtrArray *cleanup_task_array_new(void) {
    return g_ptr_array_new_with_free_func((GDestroyNotify)cleanup_task_free);
}

static void queue_cleanup_task(GPtrArray *tasks, const char *path, CleanupKind kind) {
    if (!tasks || !path || !*path) {
        return;
    }

    CleanupTask *task = g_new0(CleanupTask, 1);
    task->path = g_strdup(path);
    task->kind = kind;
    g_ptr_array_add(tasks, task);
}

static void remove_cache_artifacts_for_path(const char *path) {
    if (!path || !*path) {
        return;
    }

    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
    const char *cache_base = g_get_user_cache_dir();
    char *cache_path = dict_cache_path_for(path);
    char *resource_dir = g_build_filename(cache_base, "diction", "resources", hash, NULL);
    remove_path_recursive(cache_path);
    remove_path_recursive(resource_dir);

    /* Clean up XDXF-specific cache artifacts */
    char *xdxf_res = g_strdup_printf("%s.res", cache_path);
    char *xdxf_meta = g_strdup_printf("%s.meta", cache_path);
    char *cache_tmp = g_strdup_printf("%s.tmp", cache_path);
    remove_path_recursive(xdxf_res);
    remove_path_recursive(xdxf_meta);
    remove_path_recursive(cache_tmp);
    g_free(xdxf_res);
    g_free(xdxf_meta);
    g_free(cache_tmp);

    /* Also clean up icon cache entries */
    char *icon_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
    char *icon_path = g_build_filename(cache_base, "diction", "icons", icon_hash, NULL);
    char *icon_png = g_strdup_printf("%s.png", icon_path);
    if (g_file_test(icon_png, G_FILE_TEST_EXISTS)) {
        g_unlink(icon_png);
    }
    g_free(icon_png);
    g_free(icon_path);
    g_free(icon_hash);

    g_free(cache_path);
    g_free(resource_dir);
    g_free(hash);

    if (ends_with_ci(path, ".mdx")) {
        char *basename = g_path_get_basename(path);
        char *stem = strip_suffix_dup(basename, ".mdx");
        char *mdx_resource_dir = g_build_filename(cache_base, "diction", "resources", stem, NULL);
        remove_path_recursive(mdx_resource_dir);
        g_free(mdx_resource_dir);
        g_free(stem);
        g_free(basename);
    }
}

static void remove_dictionary_payload_for_path(const char *path) {
    if (!path || !*path) {
        return;
    }

    DictFormat fmt = dict_detect_format(path);
    remove_path_recursive(path);

    if (fmt == DICT_FORMAT_STARDICT) {
        char *base = strip_suffix_dup(path, ".ifo");
        const char *suffixes[] = {
            ".idx", ".idx.gz", ".idx.dz",
            ".dict", ".dict.dz",
            ".syn", ".syn.gz",
            ".files", ".dict.files", ".ifo.files"
        };
        for (guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
            char *candidate = g_strconcat(base, suffixes[i], NULL);
            remove_path_recursive(candidate);
            g_free(candidate);
        }
        g_free(base);
    } else if (fmt == DICT_FORMAT_DSL) {
        char *without_dz = ends_with_ci(path, ".dsl.dz")
            ? strip_suffix_dup(path, ".dz")
            : NULL;
        const char *suffixes[] = {
            ".files", ".files.zip", ".dz.files", ".dz.files.zip"
        };

        for (guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
            char *candidate = g_strconcat(path, suffixes[i], NULL);
            remove_path_recursive(candidate);
            g_free(candidate);
        }

        if (without_dz) {
            char *candidate = g_strconcat(without_dz, ".files", NULL);
            remove_path_recursive(candidate);
            g_free(candidate);

            candidate = g_strconcat(without_dz, ".files.zip", NULL);
            remove_path_recursive(candidate);
            g_free(candidate);
            g_free(without_dz);
        }
    } else if (fmt == DICT_FORMAT_BGL) {
        char *base = strip_suffix_dup(path, ".bgl");
        char *resource_dir = g_strconcat(base, ".files", NULL);
        remove_path_recursive(resource_dir);
        g_free(resource_dir);
        g_free(base);
    } else if (fmt == DICT_FORMAT_MDX) {
        GPtrArray *mdd_paths = settings_collect_mdx_companion_paths(path);
        for (guint i = 0; mdd_paths && i < mdd_paths->len; i++) {
            const char *candidate = g_ptr_array_index(mdd_paths, i);
            remove_path_recursive(candidate);
        }
        if (mdd_paths) {
            g_ptr_array_free(mdd_paths, TRUE);
        }
    }

    remove_cache_artifacts_for_path(path);
}

static gpointer cleanup_thread_func(gpointer user_data) {
    GPtrArray *tasks = user_data;
    if (!tasks) {
        return NULL;
    }

    for (guint i = 0; i < tasks->len; i++) {
        CleanupTask *task = g_ptr_array_index(tasks, i);
        if (!task || !task->path || !*task->path) {
            continue;
        }

        if (task->kind == CLEANUP_DELETE_PAYLOAD) {
            remove_dictionary_payload_for_path(task->path);
        } else {
            remove_cache_artifacts_for_path(task->path);
        }
    }

    g_ptr_array_free(tasks, TRUE);
    return NULL;
}

static void dispatch_cleanup_tasks(GPtrArray *tasks) {
    if (!tasks) {
        return;
    }

    if (tasks->len == 0) {
        g_ptr_array_free(tasks, TRUE);
        return;
    }

    GThread *thread = g_thread_new("settings-cleanup", cleanup_thread_func, tasks);
    g_thread_unref(thread);
}

static char *find_existing_sibling(const char *base_path, const char * const *suffixes, gsize count, gboolean require_directory) {
    for (gsize i = 0; i < count; i++) {
        char *candidate = g_strconcat(base_path, suffixes[i], NULL);
        gboolean exists = require_directory
            ? g_file_test(candidate, G_FILE_TEST_IS_DIR)
            : g_file_test(candidate, G_FILE_TEST_EXISTS);
        if (exists) {
            return candidate;
        }
        g_free(candidate);
    }

    return NULL;
}

static gboolean settings_mdx_filename_matches_numbered_mdd(const char *filename,
                                                           const char *stem) {
    if (!filename || !stem || !g_str_has_suffix(filename, ".mdd")) {
        return FALSE;
    }

    gsize stem_len = strlen(stem);
    gsize filename_len = strlen(filename);
    if (filename_len <= stem_len + 4 || strncmp(filename, stem, stem_len) != 0) {
        return FALSE;
    }

    const char *suffix = filename + stem_len;
    const char *digits = suffix;
    const char *end = filename + filename_len - 4; /* before ".mdd" */

    if (*digits == '.') {
        digits++;
    }

    if (digits >= end) {
        return FALSE;
    }

    for (const char *p = digits; p < end; p++) {
        if (!g_ascii_isdigit(*p)) {
            return FALSE;
        }
    }

    return TRUE;
}

static int settings_mdx_numbered_mdd_volume(const char *filename) {
    if (!filename || !g_str_has_suffix(filename, ".mdd")) {
        return 0;
    }

    gsize filename_len = strlen(filename);
    const char *digits_end = filename + filename_len - 4;
    const char *digits = digits_end;

    while (digits > filename && g_ascii_isdigit(*(digits - 1))) {
        digits--;
    }

    if (digits == digits_end) {
        return 0;
    }

    return atoi(digits);
}

static gint settings_mdx_companion_compare(gconstpointer a, gconstpointer b) {
    const char *path_a = *(char * const *)a;
    const char *path_b = *(char * const *)b;
    char *name_a = g_path_get_basename(path_a);
    char *name_b = g_path_get_basename(path_b);
    int vol_a = settings_mdx_numbered_mdd_volume(name_a);
    int vol_b = settings_mdx_numbered_mdd_volume(name_b);
    gint cmp = (vol_a != vol_b)
        ? ((vol_a < vol_b) ? -1 : 1)
        : g_strcmp0(name_a, name_b);

    g_free(name_a);
    g_free(name_b);
    return cmp;
}

static GPtrArray *settings_collect_mdx_companion_paths(const char *mdx_path) {
    if (!mdx_path) {
        return NULL;
    }

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    char *base = strip_suffix_dup(mdx_path, ".mdx");
    char *primary = g_strconcat(base, ".mdd", NULL);
    if (g_file_test(primary, G_FILE_TEST_EXISTS)) {
        g_ptr_array_add(paths, primary);
    } else {
        g_free(primary);
    }

    char *dir_path = g_path_get_dirname(base);
    char *stem = g_path_get_basename(base);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir) {
        GPtrArray *numbered = g_ptr_array_new_with_free_func(g_free);
        const char *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (!settings_mdx_filename_matches_numbered_mdd(name, stem)) {
                continue;
            }

            char *candidate = g_build_filename(dir_path, name, NULL);
            if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
                g_ptr_array_add(numbered, candidate);
            } else {
                g_free(candidate);
            }
        }
        g_dir_close(dir);

        g_ptr_array_sort(numbered, settings_mdx_companion_compare);
        for (guint i = 0; i < numbered->len; i++) {
            g_ptr_array_add(paths, g_ptr_array_index(numbered, i));
        }
        g_ptr_array_free(numbered, FALSE);
    }

    g_free(stem);
    g_free(dir_path);
    g_free(base);
    return paths;
}

static gboolean copy_path_recursive(const char *src, const char *dest, GError **error) {
    if (g_file_test(src, G_FILE_TEST_IS_DIR)) {
        if (g_mkdir_with_parents(dest, 0755) != 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "Failed to create directory %s", dest);
            return FALSE;
        }

        GDir *dir = g_dir_open(src, 0, error);
        if (!dir) {
            return FALSE;
        }

        gboolean ok = TRUE;
        const char *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            char *src_child = g_build_filename(src, name, NULL);
            char *dest_child = g_build_filename(dest, name, NULL);
            if (!copy_path_recursive(src_child, dest_child, error)) {
                ok = FALSE;
                g_free(src_child);
                g_free(dest_child);
                break;
            }
            g_free(src_child);
            g_free(dest_child);
        }

        g_dir_close(dir);
        return ok;
    }

    GFile *src_file = g_file_new_for_path(src);
    GFile *dest_file = g_file_new_for_path(dest);
    gboolean ok = g_file_copy(src_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, error);
    g_object_unref(src_file);
    g_object_unref(dest_file);
    return ok;
}

static char *make_import_candidate_path(const char *dest_dir, const char *stem, const char *suffix, int copy_index) {
    char *name = copy_index == 0
        ? g_strconcat(stem, suffix ? suffix : "", NULL)
        : g_strdup_printf("%s-%d%s", stem, copy_index, suffix ? suffix : "");
    char *candidate = g_build_filename(dest_dir, name, NULL);
    g_free(name);
    return candidate;
}

static char *build_unique_import_path(const char *dest_dir, const char *stem, const char *suffix) {
    for (int copy_index = 0; ; copy_index++) {
        char *candidate = make_import_candidate_path(dest_dir, stem, suffix, copy_index);
        if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) {
            return candidate;
        }
        g_free(candidate);
    }
}

char* settings_make_dictionary_id(const char *path) {
    char *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path ? path : "", -1);
    char *id = g_strndup(checksum ? checksum : "", 12);
    g_free(checksum);
    return id;
}

// Dictionary config helpers
static DictConfig* dict_config_new(const char *name, const char *path, const char *source) {
    DictConfig *cfg = g_new0(DictConfig, 1);
    cfg->id = settings_make_dictionary_id(path);
    cfg->name = g_strdup(name);
    cfg->path = g_strdup(path);
    cfg->enabled = 1;
    cfg->source = g_strdup(source ? source : "manual");
    return cfg;
}

static void dict_config_free(DictConfig *cfg) {
    if (cfg) {
        g_free(cfg->id);
        g_free(cfg->name);
        g_free(cfg->path);
        g_free(cfg->source);
        g_free(cfg);
    }
}

// Group helpers
static DictGroup* dict_group_new(const char *name) {
    DictGroup *grp = g_new0(DictGroup, 1);
    grp->id = g_compute_checksum_for_string(G_CHECKSUM_SHA1, name, -1);
    grp->id = g_strndup(grp->id, 8);
    grp->name = g_strdup(name);
    grp->source = g_strdup("user");
    grp->members = g_ptr_array_new_with_free_func(g_free);
    return grp;
}

static void dict_group_free(DictGroup *grp) {
    if (grp) {
        g_free(grp->id);
        g_free(grp->name);
        g_free(grp->source);
        g_ptr_array_free(grp->members, TRUE);
        g_free(grp);
    }
}

static void settings_replace_dict_id_in_groups(AppSettings *settings,
                                               const char *old_id,
                                               const char *new_id) {
    if (!settings || !old_id || !new_id) {
        return;
    }

    for (guint i = 0; i < settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(settings->dictionary_groups, i);
        for (guint j = 0; j < grp->members->len; j++) {
            char *member = g_ptr_array_index(grp->members, j);
            if (g_strcmp0(member, old_id) == 0) {
                g_free(member);
                g_ptr_array_index(grp->members, j) = g_strdup(new_id);
            }
        }
    }
}

static void settings_strip_dict_from_groups(AppSettings *settings, const char *dict_id) {
    if (!settings || !dict_id) {
        return;
    }

    for (guint i = 0; i < settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(settings->dictionary_groups, i);
        for (gint j = (gint)grp->members->len - 1; j >= 0; j--) {
            const char *member = g_ptr_array_index(grp->members, (guint)j);
            if (g_strcmp0(member, dict_id) == 0) {
                g_ptr_array_remove_index(grp->members, (guint)j);
            }
        }
    }
}

gboolean settings_is_dictionary_ignored(AppSettings *settings, const char *path) {
    if (!settings || !path || !*path || !settings->ignored_dictionary_paths) {
        return FALSE;
    }

    g_mutex_lock(&settings->mutex);
    gboolean ignored = FALSE;
    for (guint i = 0; i < settings->ignored_dictionary_paths->len; i++) {
        const char *ignored_path = g_ptr_array_index(settings->ignored_dictionary_paths, i);
        if (g_strcmp0(ignored_path, path) == 0) {
            ignored = TRUE;
            break;
        }
    }
    g_mutex_unlock(&settings->mutex);

    return ignored;
}

static void settings_set_dictionary_ignored_locked(AppSettings *settings,
                                                   const char *path,
                                                   gboolean ignored) {
    for (guint i = 0; i < settings->ignored_dictionary_paths->len; i++) {
        const char *ignored_path = g_ptr_array_index(settings->ignored_dictionary_paths, i);
        if (g_strcmp0(ignored_path, path) != 0) {
            continue;
        }

        if (!ignored) {
            g_ptr_array_remove_index(settings->ignored_dictionary_paths, i);
        }
        return;
    }

    if (ignored) {
        g_ptr_array_add(settings->ignored_dictionary_paths, g_strdup(path));
    }
}

void settings_set_dictionary_ignored(AppSettings *settings, const char *path, gboolean ignored) {
    if (!settings || !path || !*path || !settings->ignored_dictionary_paths) {
        return;
    }

    g_mutex_lock(&settings->mutex);
    settings_set_dictionary_ignored_locked(settings, path, ignored);
    g_mutex_unlock(&settings->mutex);
}

DictConfig* settings_find_dictionary_by_id(AppSettings *settings, const char *id) {
    if (!settings || !id || !settings->dictionaries) {
        return NULL;
    }

    g_mutex_lock(&settings->mutex);
    DictConfig *found = settings_find_dictionary_by_id_locked(settings, id);
    g_mutex_unlock(&settings->mutex);

    return found;
}

DictConfig* settings_find_dictionary_by_path(AppSettings *settings, const char *path) {
    if (!settings || !path || !settings->dictionaries) {
        return NULL;
    }

    g_mutex_lock(&settings->mutex);
    DictConfig *found = settings_find_dictionary_by_path_locked(settings, path);
    g_mutex_unlock(&settings->mutex);

    return found;
}

char* settings_dup_dictionary_name_by_id(AppSettings *settings, const char *id) {
    if (!settings || !id) {
        return NULL;
    }

    g_mutex_lock(&settings->mutex);
    DictConfig *cfg = settings_find_dictionary_by_id_locked(settings, id);
    char *name = cfg && cfg->name ? g_strdup(cfg->name) : NULL;
    g_mutex_unlock(&settings->mutex);
    return name;
}

gboolean settings_dictionary_enabled_by_path(AppSettings *settings, const char *path, gboolean default_enabled) {
    if (!settings || !path || !*path) {
        return default_enabled;
    }

    g_mutex_lock(&settings->mutex);
    DictConfig *cfg = settings_find_dictionary_by_path_locked(settings, path);
    gboolean enabled = cfg ? (cfg->enabled != 0) : default_enabled;
    g_mutex_unlock(&settings->mutex);
    return enabled;
}

gboolean settings_set_dictionary_enabled_by_id(AppSettings *settings, const char *id, gboolean enabled) {
    if (!settings || !id) {
        return FALSE;
    }

    g_mutex_lock(&settings->mutex);
    DictConfig *cfg = settings_find_dictionary_by_id_locked(settings, id);
    if (!cfg) {
        g_mutex_unlock(&settings->mutex);
        return FALSE;
    }

    cfg->enabled = enabled ? 1 : 0;
    g_mutex_unlock(&settings->mutex);
    return TRUE;
}

void settings_upsert_dictionary(AppSettings *settings, const char *name, const char *path, const char *source) {
    if (!settings || !path) return;

    g_mutex_lock(&settings->mutex);
    DictConfig *existing = settings_find_dictionary_by_path_locked(settings, path);
    if (existing) {
        // Update name if it changed
        if (name && g_strcmp0(existing->name, name) != 0) {
            g_free(existing->name);
            existing->name = g_strdup(name);
        }
        g_mutex_unlock(&settings->mutex);
        return;
    }

    DictConfig *cfg = dict_config_new(name, path, source);
    g_ptr_array_add(settings->dictionaries, cfg);
    g_mutex_unlock(&settings->mutex);
}

void settings_prune_directory_dictionaries(AppSettings *settings, GHashTable *loaded_paths) {
    if (!settings || !loaded_paths) return;
    
    g_mutex_lock(&settings->mutex);
    GPtrArray *cleanup_tasks = cleanup_task_array_new();
    for (gint i = (gint)settings->dictionaries->len - 1; i >= 0; i--) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, (guint)i);
        if (g_strcmp0(cfg->source, "directory") != 0) {
            continue;
        }
        if (loaded_paths && g_hash_table_contains(loaded_paths, cfg->path)) {
            continue;
        }

        queue_cleanup_task(cleanup_tasks, cfg->path, CLEANUP_CACHE_ONLY);
        settings_strip_dict_from_groups(settings, cfg->id);
        g_ptr_array_remove_index(settings->dictionaries, (guint)i);
    }
    g_mutex_unlock(&settings->mutex);

    dispatch_cleanup_tasks(cleanup_tasks);
}

// Settings load/save
AppSettings* settings_load(void) {
    AppSettings *settings = g_new0(AppSettings, 1);
    settings->dictionaries = g_ptr_array_new_with_free_func((GDestroyNotify)dict_config_free);
    settings->dictionary_dirs = g_ptr_array_new_with_free_func(g_free);
    settings->ignored_dictionary_paths = g_ptr_array_new_with_free_func(g_free);
    settings->dictionary_groups = g_ptr_array_new_with_free_func((GDestroyNotify)dict_group_free);
    settings->theme = g_strdup("system");
    settings->font_family = g_strdup("sans-serif");
    settings->font_size = 16;
    settings->color_theme = g_strdup("default");
    settings->render_style = g_strdup("diction");
    settings->scan_popup_enabled = FALSE;
    settings->scan_selection_enabled = TRUE;
    settings->scan_clipboard_enabled = FALSE;
    settings->tray_icon_enabled = FALSE;
    settings->close_to_tray = FALSE;
    settings->scan_popup_delay_ms = 500;
    settings->scan_modifier_key = g_strdup("none");
    settings->global_shortcut = g_strdup("");
    g_mutex_init(&settings->mutex);

    char *path = get_settings_file_path();
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return settings;
    }

    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    char *json_data = NULL;
    gsize json_len = 0;

    if (!g_file_get_contents(path, &json_data, &json_len, &error)) {
        g_warning("Failed to read settings file: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(path);
        return settings;
    }

    /* Heal potentially corrupt JSON files containing invalid UTF-8 characters */
    char *valid_json = g_utf8_make_valid(json_data, json_len);
    g_free(json_data);

    if (!json_parser_load_from_data(parser, valid_json, -1, &error)) {
        g_warning("Failed to parse settings: %s", error->message);
        g_error_free(error);
        g_free(valid_json);
        g_object_unref(parser);
        g_free(path);
        return settings;
    }
    g_free(valid_json);

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        g_free(path);
        return settings;
    }

    JsonObject *obj = json_node_get_object(root);

    // Theme
    const char *theme = json_object_get_string_member(obj, "theme");
    if (theme) {
        g_free(settings->theme);
        settings->theme = g_strdup(theme);
    }

    // Font
    const char *font_family = json_object_get_string_member(obj, "font_family");
    if (font_family) {
        g_free(settings->font_family);
        settings->font_family = g_strdup(font_family);
    }
    if (json_object_has_member(obj, "font_size")) {
        settings->font_size = (int)json_object_get_int_member(obj, "font_size");
        if (settings->font_size < 8 || settings->font_size > 48)
            settings->font_size = 16;
    }

    // Color Theme
    const char *color_theme = json_object_get_string_member(obj, "color_theme");
    if (color_theme) {
        g_free(settings->color_theme);
        settings->color_theme = g_strdup(color_theme);
    }

    const char *render_style = json_object_get_string_member(obj, "render_style");
    if (render_style && *render_style) {
        g_free(settings->render_style);
        settings->render_style = g_strdup(render_style);
    }

    // Scan popup / tray icon settings
    if (json_object_has_member(obj, "scan_popup_enabled"))
        settings->scan_popup_enabled = json_object_get_boolean_member(obj, "scan_popup_enabled");
    if (json_object_has_member(obj, "scan_selection_enabled"))
        settings->scan_selection_enabled = json_object_get_boolean_member(obj, "scan_selection_enabled");
    if (json_object_has_member(obj, "scan_clipboard_enabled"))
        settings->scan_clipboard_enabled = json_object_get_boolean_member(obj, "scan_clipboard_enabled");
    if (json_object_has_member(obj, "tray_icon_enabled"))
        settings->tray_icon_enabled = json_object_get_boolean_member(obj, "tray_icon_enabled");
    if (json_object_has_member(obj, "close_to_tray"))
        settings->close_to_tray = json_object_get_boolean_member(obj, "close_to_tray");
    if (json_object_has_member(obj, "scan_popup_delay_ms")) {
        settings->scan_popup_delay_ms = (int)json_object_get_int_member(obj, "scan_popup_delay_ms");
        if (settings->scan_popup_delay_ms < 100) settings->scan_popup_delay_ms = 100;
        if (settings->scan_popup_delay_ms > 5000) settings->scan_popup_delay_ms = 5000;
    }
    const char *scan_modifier_key = json_object_has_member(obj, "scan_modifier_key")
        ? json_object_get_string_member(obj, "scan_modifier_key")
        : NULL;
    if (scan_modifier_key && (
            g_strcmp0(scan_modifier_key, "none") == 0 ||
            g_strcmp0(scan_modifier_key, "ctrl") == 0 ||
            g_strcmp0(scan_modifier_key, "alt") == 0 ||
            g_strcmp0(scan_modifier_key, "meta") == 0)) {
        g_free(settings->scan_modifier_key);
        settings->scan_modifier_key = g_strdup(scan_modifier_key);
    }
    const char *global_shortcut = json_object_get_string_member(obj, "global_shortcut");
    if (global_shortcut) {
        g_free(settings->global_shortcut);
        settings->global_shortcut = g_strdup(global_shortcut);
    }

    // Dictionary directories
    JsonArray *dirs = json_object_has_member(obj, "dictionary_dirs")
        ? json_object_get_array_member(obj, "dictionary_dirs")
        : NULL;
    if (dirs) {
        for (guint i = 0; i < json_array_get_length(dirs); i++) {
            const char *dir = json_array_get_string_element(dirs, i);
            g_ptr_array_add(settings->dictionary_dirs, g_strdup(dir));
        }
    }

    JsonArray *ignored_paths = json_object_has_member(obj, "ignored_dictionary_paths")
        ? json_object_get_array_member(obj, "ignored_dictionary_paths")
        : NULL;
    if (ignored_paths) {
        for (guint i = 0; i < json_array_get_length(ignored_paths); i++) {
            const char *ignored_path = json_array_get_string_element(ignored_paths, i);
            if (ignored_path && *ignored_path) {
                g_ptr_array_add(settings->ignored_dictionary_paths, g_strdup(ignored_path));
            }
        }
    }

    // Dictionaries
    JsonArray *dicts = json_object_has_member(obj, "dictionaries")
        ? json_object_get_array_member(obj, "dictionaries")
        : NULL;
    if (dicts) {
        for (guint i = 0; i < json_array_get_length(dicts); i++) {
            JsonNode *node = json_array_get_element(dicts, i);
            if (JSON_NODE_HOLDS_OBJECT(node)) {
                JsonObject *dobj = json_node_get_object(node);
                const char *name = json_object_get_string_member(dobj, "name");
                const char *path = json_object_get_string_member(dobj, "path");
                const char *source = json_object_get_string_member(dobj, "source");
                int enabled = json_object_get_int_member(dobj, "enabled");
                
                if (path && g_path_is_absolute(path)) {
                    DictConfig *cfg = dict_config_new(name ? name : path, path, source);
                    cfg->enabled = enabled;
                    g_ptr_array_add(settings->dictionaries, cfg);
                }
            }
        }
    }

    // Groups
    JsonArray *groups = json_object_has_member(obj, "dictionary_groups")
        ? json_object_get_array_member(obj, "dictionary_groups")
        : NULL;
    if (groups) {
        for (guint i = 0; i < json_array_get_length(groups); i++) {
            JsonNode *node = json_array_get_element(groups, i);
            if (JSON_NODE_HOLDS_OBJECT(node)) {
                JsonObject *gobj = json_node_get_object(node);
                const char *gname = json_object_get_string_member(gobj, "name");
                if (gname) {
                    DictGroup *grp = dict_group_new(gname);
                    const char *gsource = json_object_has_member(gobj, "source") ? json_object_get_string_member(gobj, "source") : NULL;
                    if (gsource) {
                        g_free(grp->source);
                        grp->source = g_strdup(gsource);
                    }
                    JsonArray *members = json_object_has_member(gobj, "members")
                        ? json_object_get_array_member(gobj, "members")
                        : NULL;
                    if (members) {
                        for (guint j = 0; j < json_array_get_length(members); j++) {
                            g_ptr_array_add(grp->members, g_strdup(json_array_get_string_element(members, j)));
                        }
                    }
                    g_ptr_array_add(settings->dictionary_groups, grp);
                }
            }
        }
    }

    g_object_unref(parser);
    g_free(path);
    return settings;
}

void settings_save(AppSettings *settings) {
    if (!settings) return;
    g_mutex_lock(&settings->mutex);

    ensure_config_dir();
    char *path = get_settings_file_path();

    JsonObject *root = json_object_new();
    json_object_set_string_member(root, "theme", settings->theme);
    json_object_set_string_member(root, "font_family",
        settings->font_family ? settings->font_family : "sans-serif");
    json_object_set_int_member(root, "font_size", settings->font_size > 0 ? settings->font_size : 16);
    json_object_set_string_member(root, "color_theme",
        settings->color_theme ? settings->color_theme : "default");
    json_object_set_string_member(root, "render_style",
        settings->render_style ? settings->render_style : "diction");

    // Scan popup / tray icon settings
    json_object_set_boolean_member(root, "scan_popup_enabled", settings->scan_popup_enabled);
    json_object_set_boolean_member(root, "scan_selection_enabled", settings->scan_selection_enabled);
    json_object_set_boolean_member(root, "scan_clipboard_enabled", settings->scan_clipboard_enabled);
    json_object_set_boolean_member(root, "tray_icon_enabled", settings->tray_icon_enabled);
    json_object_set_boolean_member(root, "close_to_tray", settings->close_to_tray);
    json_object_set_int_member(root, "scan_popup_delay_ms",
        settings->scan_popup_delay_ms > 0 ? settings->scan_popup_delay_ms : 500);
    json_object_set_string_member(root, "scan_modifier_key",
        settings->scan_modifier_key ? settings->scan_modifier_key : "none");
    json_object_set_string_member(root, "global_shortcut",
        settings->global_shortcut ? settings->global_shortcut : "");

    // Dictionary directories
    JsonArray *dirs = json_array_new();
    for (guint i = 0; i < settings->dictionary_dirs->len; i++) {
        json_array_add_string_element(dirs, g_ptr_array_index(settings->dictionary_dirs, i));
    }
    json_object_set_array_member(root, "dictionary_dirs", dirs);

    JsonArray *ignored_paths = json_array_new();
    for (guint i = 0; i < settings->ignored_dictionary_paths->len; i++) {
        json_array_add_string_element(ignored_paths,
            g_ptr_array_index(settings->ignored_dictionary_paths, i));
    }
    json_object_set_array_member(root, "ignored_dictionary_paths", ignored_paths);

    // Dictionaries
    JsonArray *dicts = json_array_new();
    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        JsonObject *dobj = json_object_new();
        json_object_set_string_member(dobj, "id", cfg->id);
        json_object_set_string_member(dobj, "name", cfg->name);
        json_object_set_string_member(dobj, "path", cfg->path);
        json_object_set_int_member(dobj, "enabled", cfg->enabled);
        json_object_set_string_member(dobj, "source", cfg->source);
        json_array_add_object_element(dicts, dobj);
    }
    json_object_set_array_member(root, "dictionaries", dicts);

    // Groups
    JsonArray *groups = json_array_new();
    for (guint i = 0; i < settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(settings->dictionary_groups, i);
        if (g_strcmp0(grp->source, "guessed") == 0) {
            continue;
        }
        JsonObject *gobj = json_object_new();
        json_object_set_string_member(gobj, "id", grp->id);
        json_object_set_string_member(gobj, "name", grp->name);
        json_object_set_string_member(gobj, "source", grp->source);
        JsonArray *members = json_array_new();
        for (guint j = 0; j < grp->members->len; j++) {
            json_array_add_string_element(members, g_ptr_array_index(grp->members, j));
        }
        json_object_set_array_member(gobj, "members", members);
        json_array_add_object_element(groups, gobj);
    }
    json_object_set_array_member(root, "dictionary_groups", groups);

    JsonNode *node = json_node_init_object(json_node_alloc(), root);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, node);
    
    // Release lock before I/O if possible? No, we need the JSON node to be stable.
    // json_generator_to_file might be slow, but it's safe since it's on the main thread mostly.
    json_generator_to_file(gen, path, NULL);

    g_object_unref(gen);
    json_node_free(node);
    json_object_unref(root);
    g_free(path);

    g_mutex_unlock(&settings->mutex);
}

void settings_free(AppSettings *settings) {
    if (settings) {
        g_ptr_array_free(settings->dictionaries, TRUE);
        g_ptr_array_free(settings->dictionary_dirs, TRUE);
        g_ptr_array_free(settings->ignored_dictionary_paths, TRUE);
        g_ptr_array_free(settings->dictionary_groups, TRUE);
        g_free(settings->theme);
        g_free(settings->font_family);
        g_free(settings->color_theme);
        g_free(settings->render_style);
        g_free(settings->scan_modifier_key);
        g_free(settings->global_shortcut);
        g_mutex_clear(&settings->mutex);
        g_free(settings);
    }
}

// Helper functions
void settings_add_directory(AppSettings *settings, const char *path) {
    if (!settings || !path || !*path) {
        return;
    }

    g_mutex_lock(&settings->mutex);
    // Check if already exists
    for (guint i = 0; i < settings->dictionary_dirs->len; i++) {
        if (strcmp(g_ptr_array_index(settings->dictionary_dirs, i), path) == 0) {
            g_mutex_unlock(&settings->mutex);
            return;
        }
    }
    g_ptr_array_add(settings->dictionary_dirs, g_strdup(path));
    g_mutex_unlock(&settings->mutex);
}

void settings_remove_directory(AppSettings *settings, const char *path) {
    if (!settings || !path || !*path) {
        return;
    }

    g_mutex_lock(&settings->mutex);
    for (guint i = 0; i < settings->dictionary_dirs->len; i++) {
        if (strcmp(g_ptr_array_index(settings->dictionary_dirs, i), path) == 0) {
            // g_ptr_array_remove_index calls g_free via the array's destroy func
            g_ptr_array_remove_index(settings->dictionary_dirs, i);
            GPtrArray *cleanup_tasks = cleanup_task_array_new();
            for (gint j = (gint)settings->dictionaries->len - 1; j >= 0; j--) {
                DictConfig *cfg = g_ptr_array_index(settings->dictionaries, (guint)j);
                if (!path_is_inside_dir(cfg->path, path)) {
                    continue;
                }

                queue_cleanup_task(cleanup_tasks, cfg->path, CLEANUP_CACHE_ONLY);
                settings_strip_dict_from_groups(settings, cfg->id);
                g_ptr_array_remove_index(settings->dictionaries, (guint)j);
            }

            for (gint j = (gint)settings->ignored_dictionary_paths->len - 1; j >= 0; j--) {
                const char *ignored_path = g_ptr_array_index(settings->ignored_dictionary_paths, (guint)j);
                if (path_is_inside_dir(ignored_path, path)) {
                    queue_cleanup_task(cleanup_tasks, ignored_path, CLEANUP_CACHE_ONLY);
                    g_ptr_array_remove_index(settings->ignored_dictionary_paths, (guint)j);
                }
            }

            g_mutex_unlock(&settings->mutex);
            settings_save(settings);
            dispatch_cleanup_tasks(cleanup_tasks);
            return;
        }
    }
    g_mutex_unlock(&settings->mutex);
}

gboolean settings_import_dictionary(AppSettings *settings, const char *src_path) {
    if (!settings || !src_path || !g_file_test(src_path, G_FILE_TEST_EXISTS)) {
        return FALSE;
    }

    char *dest_dir = get_dictionary_data_dir();
    g_mkdir_with_parents(dest_dir, 0755);

    g_mutex_lock(&settings->mutex);
    gboolean had_existing_cfg = settings_find_dictionary_by_path_locked(settings, src_path) != NULL;
    g_mutex_unlock(&settings->mutex);
    gboolean src_in_managed_dir = path_is_inside_dir(src_path, dest_dir);
    gboolean src_in_watched_dir = settings_path_is_in_directory_list(settings, src_path);

    if (src_in_managed_dir) {
        char *name = settings_resolve_dictionary_name(src_path);

        if (had_existing_cfg) {
            g_mutex_lock(&settings->mutex);
            DictConfig *existing_cfg = settings_find_dictionary_by_path_locked(settings, src_path);
            if (existing_cfg) {
                char *old_id = g_strdup(existing_cfg->id);
                g_free(existing_cfg->id);
                existing_cfg->id = settings_make_dictionary_id(src_path);
                g_free(existing_cfg->name);
                existing_cfg->name = g_strdup(name);
                g_free(existing_cfg->source);
                existing_cfg->source = g_strdup("imported");
                settings_replace_dict_id_in_groups(settings, old_id, existing_cfg->id);
                g_free(old_id);
            } else {
                g_ptr_array_add(settings->dictionaries, dict_config_new(name, src_path, "imported"));
            }
            g_mutex_unlock(&settings->mutex);
        } else {
            settings_upsert_dictionary(settings, name, src_path, "imported");
        }

        settings_set_dictionary_ignored(settings, src_path, FALSE);
        settings_save(settings);
        g_free(name);
        g_free(dest_dir);
        return TRUE;
    }

    DictFormat fmt = dict_detect_format(src_path);
    const char *primary_suffix = NULL;
    switch (fmt) {
        case DICT_FORMAT_DSL:
            primary_suffix = ends_with_ci(src_path, ".dsl.dz") ? ".dsl.dz" : ".dsl";
            break;
        case DICT_FORMAT_STARDICT:
            primary_suffix = ".ifo";
            break;
        case DICT_FORMAT_MDX:
            primary_suffix = ".mdx";
            break;
        case DICT_FORMAT_BGL:
            primary_suffix = ".bgl";
            break;
        default:
            break;
    }

    char *basename = g_path_get_basename(src_path);
    char *stem = (primary_suffix && ends_with_ci(basename, primary_suffix))
        ? g_strndup(basename, strlen(basename) - strlen(primary_suffix))
        : strip_suffix_dup(basename, strrchr(basename, '.') ? strrchr(basename, '.') : "");
    char *dest = build_unique_import_path(dest_dir, stem, primary_suffix ? primary_suffix : "");
    GError *err = NULL;
    gboolean ok = copy_path_recursive(src_path, dest, &err);

    if (ok && fmt == DICT_FORMAT_STARDICT) {
        char *src_root = strip_suffix_dup(src_path, ".ifo");
        char *dest_root = strip_suffix_dup(dest, ".ifo");
        const char *idx_suffixes[] = { ".idx", ".idx.gz", ".idx.dz", ".IDX", ".IDX.GZ", ".IDX.DZ" };
        const char *dict_suffixes[] = { ".dict.dz", ".dict", ".DICT.DZ", ".DICT" };
        const char *syn_suffixes[] = { ".syn", ".syn.gz", ".SYN", ".SYN.GZ" };
        const char *resource_suffixes[] = { ".files", ".dict.files", ".ifo.files" };
        char *src_idx = find_existing_sibling(src_root, idx_suffixes, G_N_ELEMENTS(idx_suffixes), FALSE);
        char *src_dict = find_existing_sibling(src_root, dict_suffixes, G_N_ELEMENTS(dict_suffixes), FALSE);

        if (!src_idx || !src_dict) {
            g_set_error(&err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "StarDict import needs matching .idx and .dict files");
            ok = FALSE;
        }

        if (ok) {
            char *dest_idx = g_strconcat(dest_root, src_idx + strlen(src_root), NULL);
            char *dest_dict = g_strconcat(dest_root, src_dict + strlen(src_root), NULL);
            ok = copy_path_recursive(src_idx, dest_idx, &err) &&
                 copy_path_recursive(src_dict, dest_dict, &err);
            g_free(dest_idx);
            g_free(dest_dict);
        }

        if (ok) {
            char *src_syn = find_existing_sibling(src_root, syn_suffixes, G_N_ELEMENTS(syn_suffixes), FALSE);
            if (src_syn) {
                char *dest_syn = g_strconcat(dest_root, src_syn + strlen(src_root), NULL);
                ok = copy_path_recursive(src_syn, dest_syn, &err);
                g_free(dest_syn);
                g_free(src_syn);
            }
        }

        if (ok) {
            char *src_resource = find_existing_sibling(src_root, resource_suffixes, G_N_ELEMENTS(resource_suffixes), TRUE);
            if (src_resource) {
                char *dest_resource = g_strconcat(dest_root, src_resource + strlen(src_root), NULL);
                ok = copy_path_recursive(src_resource, dest_resource, &err);
                g_free(dest_resource);
                g_free(src_resource);
            }
        }

        g_free(src_idx);
        g_free(src_dict);
        g_free(src_root);
        g_free(dest_root);
    } else if (ok && fmt == DICT_FORMAT_DSL) {
        char *without_dz_src = ends_with_ci(src_path, ".dsl.dz") ? strip_suffix_dup(src_path, ".dz") : NULL;
        char *without_dz_dest = ends_with_ci(dest, ".dsl.dz") ? strip_suffix_dup(dest, ".dz") : NULL;
        struct {
            const char *src_base;
            const char *src_suffix;
            const char *dest_base;
            const char *dest_suffix;
        } candidates[] = {
            { src_path, ".files", dest, ".files" },
            { src_path, ".files.zip", dest, ".files.zip" },
            { src_path, ".dz.files", dest, ".dz.files" },
            { src_path, ".dz.files.zip", dest, ".dz.files.zip" },
            { without_dz_src, ".files", without_dz_dest, ".files" },
            { without_dz_src, ".files.zip", without_dz_dest, ".files.zip" },
        };

        for (guint i = 0; ok && i < G_N_ELEMENTS(candidates); i++) {
            if (!candidates[i].src_base || !candidates[i].dest_base) {
                continue;
            }

            char *src_companion = g_strconcat(candidates[i].src_base, candidates[i].src_suffix, NULL);
            if (g_file_test(src_companion, G_FILE_TEST_EXISTS)) {
                char *dest_companion = g_strconcat(candidates[i].dest_base, candidates[i].dest_suffix, NULL);
                ok = copy_path_recursive(src_companion, dest_companion, &err);
                g_free(dest_companion);
            }
            g_free(src_companion);
        }

        g_free(without_dz_src);
        g_free(without_dz_dest);
    } else if (ok && fmt == DICT_FORMAT_BGL) {
        char *src_root = strip_suffix_dup(src_path, ".bgl");
        char *dest_root = strip_suffix_dup(dest, ".bgl");
        char *src_resource = g_strconcat(src_root, ".files", NULL);
        if (g_file_test(src_resource, G_FILE_TEST_IS_DIR)) {
            char *dest_resource = g_strconcat(dest_root, ".files", NULL);
            ok = copy_path_recursive(src_resource, dest_resource, &err);
            g_free(dest_resource);
        }
        g_free(src_resource);
        g_free(src_root);
        g_free(dest_root);
    } else if (ok && fmt == DICT_FORMAT_MDX) {
        char *src_root = strip_suffix_dup(src_path, ".mdx");
        char *dest_root = strip_suffix_dup(dest, ".mdx");
        GPtrArray *mdd_paths = settings_collect_mdx_companion_paths(src_path);
        for (guint i = 0; ok && mdd_paths && i < mdd_paths->len; i++) {
            const char *src_mdd = g_ptr_array_index(mdd_paths, i);
            char *dest_mdd = g_strconcat(dest_root, src_mdd + strlen(src_root), NULL);
            ok = copy_path_recursive(src_mdd, dest_mdd, &err);
            g_free(dest_mdd);
        }

        if (mdd_paths) {
            g_ptr_array_free(mdd_paths, TRUE);
        }
        g_free(src_root);
        g_free(dest_root);
    }

    if (!ok) {
        remove_dictionary_payload_for_path(dest);
        if (err) {
            g_error_free(err);
        }
        g_free(dest_dir);
        g_free(dest);
        g_free(stem);
        g_free(basename);
        return FALSE;
    }

    /* Attempt to copy existing cache for src -> dest (reuse cache if present) */
    char *src_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, src_path, -1);
    char *dst_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, dest, -1);
    const char *cache_base = g_get_user_cache_dir();
    char *src_cache = g_build_filename(cache_base, "diction", "dicts", src_hash, NULL);
    char *dst_cache = g_build_filename(cache_base, "diction", "dicts", dst_hash, NULL);
    g_free(src_hash);
    g_free(dst_hash);

    if (g_file_test(src_cache, G_FILE_TEST_EXISTS) && !g_file_test(dst_cache, G_FILE_TEST_EXISTS)) {
        GError *cerr = NULL;
        GFile *fcsrc = g_file_new_for_path(src_cache);
        GFile *fcdst = g_file_new_for_path(dst_cache);
        gboolean cok = g_file_copy(fcsrc, fcdst, G_FILE_COPY_NONE, NULL, NULL, NULL, &cerr);
        if (!cok && cerr) {
            g_error_free(cerr);
        }
        g_object_unref(fcsrc);
        g_object_unref(fcdst);
    }

    g_free(src_cache);
    g_free(dst_cache);

    char *name = settings_resolve_dictionary_name(dest);

    if (had_existing_cfg) {
        char *old_path = NULL;

        g_mutex_lock(&settings->mutex);
        DictConfig *existing_cfg = settings_find_dictionary_by_path_locked(settings, src_path);
        if (existing_cfg) {
            char *old_id = g_strdup(existing_cfg->id);
            old_path = g_strdup(existing_cfg->path);

            g_free(existing_cfg->id);
            existing_cfg->id = settings_make_dictionary_id(dest);
            g_free(existing_cfg->name);
            existing_cfg->name = g_strdup(name);
            g_free(existing_cfg->path);
            existing_cfg->path = g_strdup(dest);
            g_free(existing_cfg->source);
            existing_cfg->source = g_strdup("imported");
            settings_replace_dict_id_in_groups(settings, old_id, existing_cfg->id);
            g_free(old_id);
        } else {
            g_ptr_array_add(settings->dictionaries, dict_config_new(name, dest, "imported"));
        }
        g_mutex_unlock(&settings->mutex);

        if (src_in_watched_dir) {
            settings_set_dictionary_ignored(settings, old_path, TRUE);
            GPtrArray *cleanup_tasks = cleanup_task_array_new();
            queue_cleanup_task(cleanup_tasks, old_path, CLEANUP_CACHE_ONLY);
            dispatch_cleanup_tasks(cleanup_tasks);
        }

        g_free(old_path);
    } else {
        settings_set_dictionary_ignored(settings, src_path, src_in_watched_dir);
        settings_upsert_dictionary(settings, name, dest, "imported");
        if (src_in_watched_dir) {
            GPtrArray *cleanup_tasks = cleanup_task_array_new();
            queue_cleanup_task(cleanup_tasks, src_path, CLEANUP_CACHE_ONLY);
            dispatch_cleanup_tasks(cleanup_tasks);
        }
    }

    settings_save(settings);
    g_free(name);
    g_free(dest_dir);
    g_free(dest);
    g_free(stem);
    g_free(basename);
    return TRUE;
}

void settings_add_dictionary(AppSettings *settings, const char *name, const char *path) {
    settings_set_dictionary_ignored(settings, path, FALSE);
    settings_upsert_dictionary(settings, name, path, "manual");
}

void settings_remove_dictionary(AppSettings *settings, const char *id) {
    if (!settings || !id) return;
    g_mutex_lock(&settings->mutex);
    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (strcmp(cfg->id, id) == 0) {
            char *managed_dir = get_dictionary_data_dir();
            gboolean managed_payload = path_is_inside_dir(cfg->path, managed_dir);
            gboolean hidden_from_watch_dirs = settings_path_is_in_directory_list(settings, cfg->path) &&
                                              !managed_payload;
            gboolean delete_payload = managed_payload &&
                                      (g_strcmp0(cfg->source, "manual") == 0 ||
                                       g_strcmp0(cfg->source, "imported") == 0);
            GPtrArray *cleanup_tasks = cleanup_task_array_new();
            queue_cleanup_task(cleanup_tasks, cfg->path,
                               delete_payload ? CLEANUP_DELETE_PAYLOAD : CLEANUP_CACHE_ONLY);
            settings_set_dictionary_ignored_locked(settings, cfg->path, hidden_from_watch_dirs);
            g_free(managed_dir);
            settings_strip_dict_from_groups(settings, cfg->id);
            g_ptr_array_remove_index(settings->dictionaries, i);
            
            g_mutex_unlock(&settings->mutex);
            settings_save(settings);
            dispatch_cleanup_tasks(cleanup_tasks);
            return;
        }
    }
    g_mutex_unlock(&settings->mutex);
}

void settings_move_dictionary(AppSettings *settings, const char *id, int direction) {
    if (!settings || !id) return;
    g_mutex_lock(&settings->mutex);
    int idx = -1;
    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (strcmp(cfg->id, id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        g_mutex_unlock(&settings->mutex);
        return;
    }

    int new_idx = idx + direction;
    if (new_idx < 0 || new_idx >= (int)settings->dictionaries->len) {
        g_mutex_unlock(&settings->mutex);
        return;
    }

    gpointer tmp = g_ptr_array_index(settings->dictionaries, idx);
    g_ptr_array_index(settings->dictionaries, idx) = g_ptr_array_index(settings->dictionaries, new_idx);
    g_ptr_array_index(settings->dictionaries, new_idx) = tmp;
    g_mutex_unlock(&settings->mutex);
}

void settings_create_group(AppSettings *settings, const char *name, GPtrArray *dict_ids) {
    DictGroup *grp = dict_group_new(name);
    for (guint i = 0; i < dict_ids->len; i++) {
        g_ptr_array_add(grp->members, g_strdup(g_ptr_array_index(dict_ids, i)));
    }
    g_ptr_array_add(settings->dictionary_groups, grp);
}

gboolean settings_upsert_guessed_group(AppSettings *settings, const char *name, const char *dict_id) {
    if (!settings || !name || !dict_id) return FALSE;

    g_mutex_lock(&settings->mutex);
    gboolean changed = FALSE;

    for (gint i = (gint)settings->dictionary_groups->len - 1; i >= 0; i--) {
        DictGroup *grp = g_ptr_array_index(settings->dictionary_groups, (guint)i);
        if (g_strcmp0(grp->source, "guessed") != 0) {
            continue;
        }

        for (gint j = (gint)grp->members->len - 1; j >= 0; j--) {
            const char *member = g_ptr_array_index(grp->members, (guint)j);
            if (g_strcmp0(member, dict_id) == 0 && g_strcmp0(grp->name, name) != 0) {
                g_ptr_array_remove_index(grp->members, (guint)j);
                changed = TRUE;
            }
        }

        if (grp->members->len == 0) {
            g_ptr_array_remove_index(settings->dictionary_groups, (guint)i);
            changed = TRUE;
        }
    }

    for (guint i = 0; i < settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(settings->dictionary_groups, i);
        if (g_strcmp0(grp->name, name) == 0 && g_strcmp0(grp->source, "guessed") == 0) {
            for (guint j = 0; j < grp->members->len; j++) {
                if (g_strcmp0(g_ptr_array_index(grp->members, j), dict_id) == 0) {
                    g_mutex_unlock(&settings->mutex);
                    return changed;
                }
            }
            g_ptr_array_add(grp->members, g_strdup(dict_id));
            g_mutex_unlock(&settings->mutex);
            return TRUE;
        }
    }

    DictGroup *grp = dict_group_new(name);
    g_free(grp->source);
    grp->source = g_strdup("guessed");
    g_ptr_array_add(grp->members, g_strdup(dict_id));
    g_ptr_array_add(settings->dictionary_groups, grp);
    g_mutex_unlock(&settings->mutex);
    return TRUE;
}

void settings_remove_group(AppSettings *settings, const char *id) {
    for (guint i = 0; i < settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(settings->dictionary_groups, i);
        if (strcmp(grp->id, id) == 0) {
            // g_ptr_array_remove_index calls dict_group_free via the array's destroy func
            g_ptr_array_remove_index(settings->dictionary_groups, i);
            return;
        }
    }
}
