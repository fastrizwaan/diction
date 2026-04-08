#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

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
    grp->members = g_ptr_array_new_with_free_func(g_free);
    return grp;
}

static void dict_group_free(DictGroup *grp) {
    if (grp) {
        g_free(grp->id);
        g_free(grp->name);
        g_ptr_array_free(grp->members, TRUE);
        g_free(grp);
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

DictConfig* settings_find_dictionary_by_id(AppSettings *settings, const char *id) {
    if (!settings || !id) {
        return NULL;
    }

    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (g_strcmp0(cfg->id, id) == 0) {
            return cfg;
        }
    }

    return NULL;
}

DictConfig* settings_find_dictionary_by_path(AppSettings *settings, const char *path) {
    if (!settings || !path) {
        return NULL;
    }

    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (g_strcmp0(cfg->path, path) == 0) {
            return cfg;
        }
    }

    return NULL;
}

void settings_upsert_dictionary(AppSettings *settings, const char *name, const char *path, const char *source) {
    if (!settings || !path || !*path) {
        return;
    }

    DictConfig *cfg = settings_find_dictionary_by_path(settings, path);
    if (cfg) {
        if (name && *name) {
            g_free(cfg->name);
            cfg->name = g_strdup(name);
        }
        if (source && *source && g_strcmp0(cfg->source, "manual") != 0) {
            g_free(cfg->source);
            cfg->source = g_strdup(source);
        }
        return;
    }

    g_ptr_array_add(settings->dictionaries, dict_config_new(name ? name : path, path, source));
}

void settings_prune_directory_dictionaries(AppSettings *settings, GHashTable *loaded_paths) {
    if (!settings) {
        return;
    }

    for (gint i = (gint)settings->dictionaries->len - 1; i >= 0; i--) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, (guint)i);
        if (g_strcmp0(cfg->source, "directory") != 0) {
            continue;
        }
        if (loaded_paths && g_hash_table_contains(loaded_paths, cfg->path)) {
            continue;
        }

        settings_strip_dict_from_groups(settings, cfg->id);
        g_ptr_array_remove_index(settings->dictionaries, (guint)i);
    }
}

// Settings load/save
AppSettings* settings_load(void) {
    AppSettings *settings = g_new0(AppSettings, 1);
    settings->dictionaries = g_ptr_array_new_with_free_func((GDestroyNotify)dict_config_free);
    settings->dictionary_dirs = g_ptr_array_new_with_free_func(g_free);
    settings->dictionary_groups = g_ptr_array_new_with_free_func((GDestroyNotify)dict_group_free);
    settings->theme = g_strdup("system");
    settings->font_family = g_strdup("sans-serif");
    settings->font_size = 16;
    settings->color_theme = g_strdup("default");
    settings->render_style = g_strdup("diction");

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

    // Dictionary directories
    JsonArray *dirs = json_object_get_array_member(obj, "dictionary_dirs");
    if (dirs) {
        for (guint i = 0; i < json_array_get_length(dirs); i++) {
            const char *dir = json_array_get_string_element(dirs, i);
            g_ptr_array_add(settings->dictionary_dirs, g_strdup(dir));
        }
    }

    // Dictionaries
    JsonArray *dicts = json_object_get_array_member(obj, "dictionaries");
    if (dicts) {
        for (guint i = 0; i < json_array_get_length(dicts); i++) {
            JsonNode *node = json_array_get_element(dicts, i);
            if (JSON_NODE_HOLDS_OBJECT(node)) {
                JsonObject *dobj = json_node_get_object(node);
                const char *name = json_object_get_string_member(dobj, "name");
                const char *path = json_object_get_string_member(dobj, "path");
                const char *source = json_object_get_string_member(dobj, "source");
                int enabled = json_object_get_int_member(dobj, "enabled");
                
                if (path) {
                    DictConfig *cfg = dict_config_new(name ? name : path, path, source);
                    cfg->enabled = enabled;
                    g_ptr_array_add(settings->dictionaries, cfg);
                }
            }
        }
    }

    // Groups
    JsonArray *groups = json_object_get_array_member(obj, "dictionary_groups");
    if (groups) {
        for (guint i = 0; i < json_array_get_length(groups); i++) {
            JsonNode *node = json_array_get_element(groups, i);
            if (JSON_NODE_HOLDS_OBJECT(node)) {
                JsonObject *gobj = json_node_get_object(node);
                const char *gname = json_object_get_string_member(gobj, "name");
                if (gname) {
                    DictGroup *grp = dict_group_new(gname);
                    JsonArray *members = json_object_get_array_member(gobj, "members");
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

    // Dictionary directories
    JsonArray *dirs = json_array_new();
    for (guint i = 0; i < settings->dictionary_dirs->len; i++) {
        json_array_add_string_element(dirs, g_ptr_array_index(settings->dictionary_dirs, i));
    }
    json_object_set_array_member(root, "dictionary_dirs", dirs);

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
        JsonObject *gobj = json_object_new();
        json_object_set_string_member(gobj, "id", grp->id);
        json_object_set_string_member(gobj, "name", grp->name);
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
    json_generator_to_file(gen, path, NULL);

    g_object_unref(gen);
    json_node_free(node);
    json_object_unref(root);
    g_free(path);
}

void settings_free(AppSettings *settings) {
    if (settings) {
        g_ptr_array_free(settings->dictionaries, TRUE);
        g_ptr_array_free(settings->dictionary_dirs, TRUE);
        g_ptr_array_free(settings->dictionary_groups, TRUE);
        g_free(settings->theme);
        g_free(settings->font_family);
        g_free(settings->color_theme);
        g_free(settings->render_style);
        g_free(settings);
    }
}

// Helper functions
void settings_add_directory(AppSettings *settings, const char *path) {
    // Check if already exists
    for (guint i = 0; i < settings->dictionary_dirs->len; i++) {
        if (strcmp(g_ptr_array_index(settings->dictionary_dirs, i), path) == 0)
            return;
    }
    g_ptr_array_add(settings->dictionary_dirs, g_strdup(path));
}

void settings_remove_directory(AppSettings *settings, const char *path) {
    for (guint i = 0; i < settings->dictionary_dirs->len; i++) {
        if (strcmp(g_ptr_array_index(settings->dictionary_dirs, i), path) == 0) {
            // g_ptr_array_remove_index calls g_free via the array's destroy func
            g_ptr_array_remove_index(settings->dictionary_dirs, i);
            /* Also clear cached files for any dictionaries that were loaded from
             * this directory and remove their settings entries. */
            for (gint j = (gint)settings->dictionaries->len - 1; j >= 0; j--) {
                DictConfig *cfg = g_ptr_array_index(settings->dictionaries, (guint)j);
                if (g_strcmp0(cfg->source, "directory") == 0 && g_str_has_prefix(cfg->path, path)) {
                    /* Remove cache and resource directories keyed by the path's SHA1 */
                    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, cfg->path, -1);
                    const char *base = g_get_user_cache_dir();
                    char *cache_path = g_build_filename(base, "diction", "dicts", hash, NULL);
                    char *res_dir = g_build_filename(base, "diction", "resources", hash, NULL);
                    g_free(hash);

                    if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
                        g_remove(cache_path);
                    }
                    if (g_file_test(res_dir, G_FILE_TEST_IS_DIR)) {
                        /* Recursively remove resource directory */
                        GDir *d = g_dir_open(res_dir, 0, NULL);
                        if (d) {
                            const char *name = NULL;
                            while ((name = g_dir_read_name(d)) != NULL) {
                                if (name[0] == '.') continue;
                                char *child = g_build_filename(res_dir, name, NULL);
                                if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
                                    /* simple recursion */
                                    GPtrArray *stack = g_ptr_array_new_with_free_func(g_free);
                                    g_ptr_array_add(stack, g_strdup(child));
                                    while (stack->len > 0) {
                                        char *p = g_ptr_array_index(stack, stack->len - 1);
                                        g_ptr_array_remove_index(stack, stack->len - 1);
                                        GDir *sub = g_dir_open(p, 0, NULL);
                                        if (!sub) { g_free(p); continue; }
                                        const char *entry;
                                        while ((entry = g_dir_read_name(sub)) != NULL) {
                                            if (entry[0] == '.') continue;
                                            char *child2 = g_build_filename(p, entry, NULL);
                                            if (g_file_test(child2, G_FILE_TEST_IS_DIR)) {
                                                g_ptr_array_add(stack, child2);
                                            } else {
                                                g_remove(child2);
                                                g_free(child2);
                                            }
                                        }
                                        g_dir_close(sub);
                                        g_rmdir(p);
                                        g_free(p);
                                    }
                                    g_ptr_array_free(stack, TRUE);
                                } else {
                                    g_remove(child);
                                    g_free(child);
                                }
                            }
                            g_dir_close(d);
                        }
                        g_rmdir(res_dir);
                    }

                    g_free(cache_path);
                    g_free(res_dir);

                    /* Remove the dict config entry */
                    settings_strip_dict_from_groups(settings, cfg->id);
                    g_ptr_array_remove_index(settings->dictionaries, (guint)j);
                }
            }
            /* Persist changes */
            settings_save(settings);
            return;
        }
    }
}

gboolean settings_import_dictionary(AppSettings *settings, const char *src_path) {
    if (!src_path || !g_file_test(src_path, G_FILE_TEST_EXISTS)) return FALSE;

    const char *data_base = g_get_user_data_dir();
    char *dest_dir = g_build_filename(data_base, "diction", "dicts", NULL);
    g_mkdir_with_parents(dest_dir, 0755);

    char *base = g_path_get_basename(src_path);
    char *dest = g_build_filename(dest_dir, base, NULL);
    g_free(base);

    /* Avoid overwriting existing file by choosing a unique name */
    int suffix = 1;
    while (g_file_test(dest, G_FILE_TEST_EXISTS)) {
        g_free(dest);
        base = g_path_get_basename(src_path);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        dest = g_strdup_printf("%s/%s-%d.%s", dest_dir, base, suffix, dot ? dot + 1 : "");
        g_free(base);
        suffix++;
    }

    GError *err = NULL;
    GFile *fsrc = g_file_new_for_path(src_path);
    GFile *fdst = g_file_new_for_path(dest);
    gboolean ok = g_file_copy(fsrc, fdst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
    g_object_unref(fsrc);
    g_object_unref(fdst);
    if (!ok) {
        if (err) g_error_free(err);
        g_free(dest_dir);
        g_free(dest);
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
        if (!cok) {
            if (cerr) g_error_free(cerr);
        }
        g_object_unref(fcsrc);
        g_object_unref(fcdst);
    }

    g_free(src_cache);
    g_free(dst_cache);

    /* Add to settings as manual dictionary */
    char *name = g_path_get_basename(dest);
    char *ext = strrchr(name, '.');
    if (ext) *ext = '\0';
    settings_add_dictionary(settings, name, dest);
    g_free(name);

    settings_save(settings);
    g_free(dest_dir);
    g_free(dest);
    return TRUE;
}

void settings_add_dictionary(AppSettings *settings, const char *name, const char *path) {
    settings_upsert_dictionary(settings, name, path, "manual");
}

void settings_remove_dictionary(AppSettings *settings, const char *id) {
    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (strcmp(cfg->id, id) == 0) {
            settings_strip_dict_from_groups(settings, cfg->id);
            // g_ptr_array_remove_index calls dict_config_free via the array's destroy func
            g_ptr_array_remove_index(settings->dictionaries, i);
            return;
        }
    }
}

void settings_move_dictionary(AppSettings *settings, const char *id, int direction) {
    int idx = -1;
    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (strcmp(cfg->id, id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return;

    int new_idx = idx + direction;
    if (new_idx < 0 || new_idx >= (int)settings->dictionaries->len) return;

    gpointer tmp = g_ptr_array_index(settings->dictionaries, idx);
    g_ptr_array_index(settings->dictionaries, idx) = g_ptr_array_index(settings->dictionaries, new_idx);
    g_ptr_array_index(settings->dictionaries, new_idx) = tmp;
}

void settings_create_group(AppSettings *settings, const char *name, GPtrArray *dict_ids) {
    DictGroup *grp = dict_group_new(name);
    for (guint i = 0; i < dict_ids->len; i++) {
        g_ptr_array_add(grp->members, g_strdup(g_ptr_array_index(dict_ids, i)));
    }
    g_ptr_array_add(settings->dictionary_groups, grp);
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
