#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

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

// Dictionary config helpers
static DictConfig* dict_config_new(const char *name, const char *path, const char *source) {
    DictConfig *cfg = g_new0(DictConfig, 1);
    cfg->id = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
    cfg->id = g_strndup(cfg->id, 12);
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

// Settings load/save
AppSettings* settings_load(void) {
    AppSettings *settings = g_new0(AppSettings, 1);
    settings->dictionaries = g_ptr_array_new_with_free_func((GDestroyNotify)dict_config_free);
    settings->dictionary_dirs = g_ptr_array_new_with_free_func(g_free);
    settings->dictionary_groups = g_ptr_array_new_with_free_func((GDestroyNotify)dict_group_free);
    settings->theme = g_strdup("system");

    char *path = get_settings_file_path();
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return settings;
    }

    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, &error)) {
        g_warning("Failed to load settings: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        g_free(path);
        return settings;
    }

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
            g_free(g_ptr_array_index(settings->dictionary_dirs, i));
            g_ptr_array_remove_index(settings->dictionary_dirs, i);
            return;
        }
    }
}

void settings_add_dictionary(AppSettings *settings, const char *name, const char *path) {
    // Check if already exists
    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (strcmp(cfg->path, path) == 0)
            return;
    }
    g_ptr_array_add(settings->dictionaries, dict_config_new(name, path, "manual"));
}

void settings_remove_dictionary(AppSettings *settings, const char *id) {
    for (guint i = 0; i < settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(settings->dictionaries, i);
        if (strcmp(cfg->id, id) == 0) {
            dict_config_free(cfg);
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
            dict_group_free(grp);
            g_ptr_array_remove_index(settings->dictionary_groups, i);
            return;
        }
    }
}
