#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include "dict-loader.h"

#define SETTINGS_FILE "settings.json"

typedef struct {
    char *id;
    char *name;
    char *path;
    int enabled;
    char *source;  // "manual" or "directory"
} DictConfig;

typedef struct {
    char *id;
    char *name;
    GPtrArray *members;  // Array of dict IDs
} DictGroup;

typedef struct {
    GPtrArray *dictionaries;      // Array of DictConfig*
    GPtrArray *dictionary_dirs;   // Array of char* (paths)
    GPtrArray *dictionary_groups; // Array of DictGroup*
    char *theme;                  // "system", "light", "dark"
} AppSettings;

// Settings management
AppSettings* settings_load(void);
void settings_save(AppSettings *settings);
void settings_free(AppSettings *settings);

// Settings dialog
GtkWidget* settings_dialog_new(GtkWindow *parent, AppSettings *settings,
                               AdwStyleManager *style_manager,
                               void (*reload_callback)(void *), void *reload_user_data);
void settings_dialog_run(GtkWidget *dialog);

// Helper functions
void settings_add_directory(AppSettings *settings, const char *path);
void settings_remove_directory(AppSettings *settings, const char *path);
void settings_add_dictionary(AppSettings *settings, const char *name, const char *path);
void settings_remove_dictionary(AppSettings *settings, const char *id);
void settings_move_dictionary(AppSettings *settings, const char *id, int direction);
void settings_create_group(AppSettings *settings, const char *name, GPtrArray *dict_ids);
void settings_remove_group(AppSettings *settings, const char *id);
char* settings_make_dictionary_id(const char *path);
DictConfig* settings_find_dictionary_by_id(AppSettings *settings, const char *id);
DictConfig* settings_find_dictionary_by_path(AppSettings *settings, const char *path);
void settings_upsert_dictionary(AppSettings *settings, const char *name, const char *path, const char *source);
void settings_prune_directory_dictionaries(AppSettings *settings, GHashTable *loaded_paths);
