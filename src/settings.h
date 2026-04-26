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
    char *source;  // "manual", "imported", or "directory"
} DictConfig;

typedef struct {
    char *id;
    char *name;
    char *source;        // "user" or "guessed"
    GPtrArray *members;  // Array of dict IDs
} DictGroup;

typedef struct {
    GPtrArray *dictionaries;      // Array of DictConfig*
    GPtrArray *dictionary_dirs;   // Array of char* (paths)
    GPtrArray *ignored_dictionary_paths; // Array of char* (paths hidden from watched folders)
    GPtrArray *dictionary_groups; // Array of DictGroup*
    char *theme;                  // "system", "light", "dark"
    char *font_family;            // e.g. "sans-serif"
    int   font_size;              // e.g. 16 (CSS px)
    char *color_theme;            // e.g. "default", "solarized", "dracula"
    char *render_style;           // e.g. "diction", "python", "goldendict-ng"
    gboolean scan_popup_enabled;  // Enable clipboard scanning
    gboolean scan_selection_enabled; // Scan PRIMARY selection changes
    gboolean scan_clipboard_enabled; // Scan regular clipboard copy changes
    gboolean tray_icon_enabled;   // Enable system tray icon
    gboolean close_to_tray;       // Close to tray instead of quitting
    int      scan_popup_delay_ms; // Debounce delay (default 500)
    char    *scan_modifier_key;   // "none", "ctrl", "alt", "meta"
    char    *global_shortcut;     // Shortcut key string (e.g. "<Ctrl>F12")
    GMutex   mutex;
} AppSettings;

// Settings management
AppSettings* settings_load(void);
void settings_save(AppSettings *settings);
void settings_free(AppSettings *settings);

// Settings dialog
GtkWidget* settings_dialog_new(GtkWindow *parent, AppSettings *settings,
                               AdwStyleManager *style_manager,
                               void (*reload_callback)(void *),
                               void (*soft_reload_callback)(void *),
                               void *user_data);
void settings_dialog_set_font_callback(GtkWidget *dialog_widget,
                                       void (*cb)(void *), void *user_data);
void settings_dialog_run(GtkWidget *dialog);

// Helper functions
void settings_add_directory(AppSettings *settings, const char *path);
void settings_remove_directory(AppSettings *settings, const char *path);
void settings_add_dictionary(AppSettings *settings, const char *name, const char *path);
void settings_remove_dictionary(AppSettings *settings, const char *id);
// Import a dictionary file into the app's data directory and add as manual dictionary.
// Returns TRUE on success and the new path will be added to settings.
gboolean settings_import_dictionary(AppSettings *settings, const char *src_path);
void settings_move_dictionary(AppSettings *settings, const char *id, int direction);
void settings_create_group(AppSettings *settings, const char *name, GPtrArray *dict_ids);
gboolean settings_upsert_guessed_group(AppSettings *settings, const char *name, const char *dict_id);
void settings_remove_group(AppSettings *settings, const char *id);
char* settings_make_dictionary_id(const char *path);
DictConfig* settings_find_dictionary_by_id(AppSettings *settings, const char *id);
DictConfig* settings_find_dictionary_by_path(AppSettings *settings, const char *path);
void settings_upsert_dictionary(AppSettings *settings, const char *name, const char *path, const char *source);
void settings_prune_directory_dictionaries(AppSettings *settings, GHashTable *loaded_paths);
gboolean settings_path_is_in_directory_list(AppSettings *settings, const char *path);
gboolean settings_is_dictionary_ignored(AppSettings *settings, const char *path);
void settings_set_dictionary_ignored(AppSettings *settings, const char *path, gboolean ignored);
char* settings_resolve_dictionary_name(const char *path);
void settings_scan_progress_notify(const char *path, int percent);
gboolean path_is_inside_dir(const char *path, const char *dir_path);
