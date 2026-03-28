#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

typedef struct {
    AdwDialog *dialog;
    AppSettings *settings;
    GtkListBox *dir_list;
    GtkListBox *dict_list;
    GtkListBox *group_list;
    GtkWidget *move_up_btn;
    GtkWidget *move_down_btn;
    GtkWidget *create_group_btn;
    char *selected_dict_id;
    GHashTable *group_selection_ids;
    AdwStyleManager *style_manager; // for live theme apply
    void (*reload_callback)(void *); // called on rescan
    void *reload_user_data;
} SettingsDialogData;

static void update_dir_list(SettingsDialogData *data);
static void update_dict_list(SettingsDialogData *data);
static void update_group_list(SettingsDialogData *data);
static void refresh_move_buttons(SettingsDialogData *data);

// Directory callbacks
typedef struct {
    SettingsDialogData *data;
    char *path;
} DirRemoveData;

static void dir_remove_data_free(DirRemoveData *d) {
    g_free(d->path);
    g_free(d);
}

static void dir_remove_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    dir_remove_data_free(data);
}

static void on_add_directory_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *chooser = GTK_FILE_DIALOG(source);
    SettingsDialogData *data = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(chooser, result, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            settings_add_directory(data->settings, path);
            update_dir_list(data);
            g_free(path);
        }
        g_object_unref(file);
    } else if (error) {
        g_error_free(error);
    }
    g_object_unref(chooser);
}

static void on_add_directory(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    GtkFileDialog *chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, "Select Dictionary Directory");
    gtk_file_dialog_select_folder(chooser, GTK_WINDOW(data->dialog), NULL,
        on_add_directory_response, data);
}

static gboolean on_remove_directory_idle(gpointer user_data) {
    SettingsDialogData *data = user_data;
    update_dir_list(data);
    return G_SOURCE_REMOVE;
}

static void on_remove_directory_clicked(GtkButton *btn, DirRemoveData *d) {
    (void)btn;
    settings_remove_directory(d->data->settings, d->path);
    g_idle_add(on_remove_directory_idle, d->data);
}

static void on_rescan_directories(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    settings_save(data->settings);
    if (data->reload_callback)
        data->reload_callback(data->reload_user_data);
}

// Dictionary callbacks
typedef struct {
    SettingsDialogData *data;
    char *id;
    int direction;
} DictMoveData;

static void dict_move_data_free(DictMoveData *d) {
    g_free(d->id);
    g_free(d);
}

static void dict_move_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    dict_move_data_free(data);
}

static void on_move_dictionary(GtkButton *btn, DictMoveData *d) {
    (void)btn;
    // Use the currently-selected dict id at click time
    const char *id = d->data->selected_dict_id;
    if (!id) return;
    settings_move_dictionary(d->data->settings, id, d->direction);
    update_dict_list(d->data);
    refresh_move_buttons(d->data);
}

static void on_add_dictionary_file_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *chooser = GTK_FILE_DIALOG(source);
    SettingsDialogData *data = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(chooser, result, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            char *name = g_path_get_basename(path);
            char *ext = strrchr(name, '.');
            if (ext) *ext = '\0';
            settings_add_dictionary(data->settings, name, path);
            update_dict_list(data);
            g_free(name);
            g_free(path);
        }
        g_object_unref(file);
    } else if (error) {
        g_error_free(error);
    }
    g_object_unref(chooser);
}

static void on_add_dictionary_file(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    GtkFileDialog *chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, "Select Dictionary File");
    
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Dictionary Files");
    gtk_file_filter_add_pattern(filter, "*.dsl");
    gtk_file_filter_add_pattern(filter, "*.dsl.dz");
    gtk_file_filter_add_pattern(filter, "*.ifo");
    gtk_file_filter_add_pattern(filter, "*.mdx");
    gtk_file_filter_add_pattern(filter, "*.bgl");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(chooser, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);
    
    gtk_file_dialog_open(chooser, GTK_WINDOW(data->dialog), NULL,
        on_add_dictionary_file_response, data);
}

typedef struct {
    SettingsDialogData *data;
    char *id;
} DictRemoveData;

static void dict_remove_data_free(DictRemoveData *d) {
    g_free(d->id);
    g_free(d);
}

static void dict_remove_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    dict_remove_data_free(data);
}

static gboolean on_remove_dictionary_idle(gpointer user_data) {
    SettingsDialogData *data = user_data;
    update_dict_list(data);
    refresh_move_buttons(data);
    return G_SOURCE_REMOVE;
}

static void on_remove_dictionary_clicked(GtkButton *btn, DictRemoveData *d) {
    (void)btn;
    settings_remove_dictionary(d->data->settings, d->id);
    g_hash_table_remove(d->data->group_selection_ids, d->id);
    g_free(d->data->selected_dict_id);
    d->data->selected_dict_id = NULL;
    g_idle_add(on_remove_dictionary_idle, d->data);
}

typedef struct {
    SettingsDialogData *data;
    DictConfig *cfg;
} DictSwitchData;

static void dict_switch_data_free(DictSwitchData *d) {
    g_free(d);
}

static void dict_switch_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    dict_switch_data_free(data);
}

static gboolean on_dict_switch_state(GtkSwitch *sw, gboolean state, DictSwitchData *sd) {
    (void)sw;
    sd->cfg->enabled = state ? 1 : 0;
    return FALSE; // allow GtkSwitch to update its visual state
}

static void on_dict_row_selected(GtkListBox *list, GtkListBoxRow *row, SettingsDialogData *data) {
    (void)list;
    g_free(data->selected_dict_id);
    data->selected_dict_id = NULL;
    
    if (row) {
        const char *id = g_object_get_data(G_OBJECT(row), "dict-id");
        if (id) {
            data->selected_dict_id = g_strdup(id);
        }
    }
    refresh_move_buttons(data);
}

static void on_group_select_toggled(GtkCheckButton *btn, gpointer user_data) {
    SettingsDialogData *data = user_data;
    const char *dict_id = g_object_get_data(G_OBJECT(btn), "dict-id");
    if (!dict_id) {
        return;
    }

    if (gtk_check_button_get_active(btn)) {
        g_hash_table_add(data->group_selection_ids, g_strdup(dict_id));
    } else {
        g_hash_table_remove(data->group_selection_ids, dict_id);
    }

    refresh_move_buttons(data);
}

static void refresh_move_buttons(SettingsDialogData *data) {
    int has_selection = (data->selected_dict_id != NULL);
    int can_move_up = 0, can_move_down = 0;
    int has_group_selection = data->group_selection_ids && g_hash_table_size(data->group_selection_ids) > 0;
    
    if (has_selection) {
        for (guint i = 0; i < data->settings->dictionaries->len; i++) {
            DictConfig *cfg = g_ptr_array_index(data->settings->dictionaries, i);
            if (strcmp(cfg->id, data->selected_dict_id) == 0) {
                can_move_up = (i > 0);
                can_move_down = (i < data->settings->dictionaries->len - 1);
                break;
            }
        }
    }
    
    gtk_widget_set_sensitive(data->move_up_btn, has_selection && can_move_up);
    gtk_widget_set_sensitive(data->move_down_btn, has_selection && can_move_down);
    gtk_widget_set_sensitive(data->create_group_btn, has_group_selection);
}

// Group callbacks
typedef struct {
    SettingsDialogData *data;
    char *id;
} GroupRemoveData;

static void group_remove_data_free(GroupRemoveData *d) {
    g_free(d->id);
    g_free(d);
}

static void group_remove_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    group_remove_data_free(data);
}

static gboolean on_remove_group_idle(gpointer user_data) {
    SettingsDialogData *data = user_data;
    update_group_list(data);
    return G_SOURCE_REMOVE;
}

static void on_remove_group_clicked(GtkButton *btn, GroupRemoveData *d) {
    (void)btn;
    settings_remove_group(d->data->settings, d->id);
    g_idle_add(on_remove_group_idle, d->data);
}

static void on_create_group_response(AdwAlertDialog *dialog, const char *response, GtkEntry *entry) {
    SettingsDialogData *data = g_object_get_data(G_OBJECT(dialog), "settings-data");

    if (strcmp(response, "create") == 0) {
        const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (name && strlen(name) > 0) {
            GPtrArray *ids = g_ptr_array_new();
            GHashTableIter iter;
            gpointer key = NULL;
            g_hash_table_iter_init(&iter, data->group_selection_ids);
            while (g_hash_table_iter_next(&iter, &key, NULL)) {
                g_ptr_array_add(ids, key);
            }
            settings_create_group(data->settings, name, ids);
            g_ptr_array_free(ids, FALSE);
            g_hash_table_remove_all(data->group_selection_ids);
            update_group_list(data);
            update_dict_list(data);
            refresh_move_buttons(data);
        }
    }
}

static void on_create_group_from_selected(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    if (!data->group_selection_ids || g_hash_table_size(data->group_selection_ids) == 0) return;

    AdwDialog *dialog = adw_alert_dialog_new("Create Group", NULL);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), "Enter a name for the new dictionary group:");

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Group name");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);

    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
        "cancel", "_Cancel",
        "create", "_Create",
        NULL);

    g_object_set_data(G_OBJECT(dialog), "settings-data", data);
    g_signal_connect(dialog, "response", G_CALLBACK(on_create_group_response), entry);

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(data->dialog));
}

// UI update functions
static void update_dir_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->dir_list)))) {
        gtk_list_box_remove(data->dir_list, child);
    }

    if (data->settings->dictionary_dirs->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No directories configured");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Add one or more folders containing dictionaries");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(data->dir_list, GTK_WIDGET(row));
        return;
    }

    for (guint i = 0; i < data->settings->dictionary_dirs->len; i++) {
        const char *path = g_ptr_array_index(data->settings->dictionary_dirs, i);
        char *name = g_path_get_basename(path);

        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), path);
        g_free(name);

        GtkWidget *remove_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(remove_btn, "flat");
        gtk_widget_add_css_class(remove_btn, "error");
        gtk_widget_set_valign(remove_btn, GTK_ALIGN_CENTER);

        DirRemoveData *rd = g_new(DirRemoveData, 1);
        rd->data = data;
        rd->path = g_strdup(path);
        g_signal_connect_data(remove_btn, "clicked", G_CALLBACK(on_remove_directory_clicked),
            rd, dir_remove_data_destroy, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_btn);
        gtk_list_box_append(data->dir_list, GTK_WIDGET(row));
    }
}

static void update_dict_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->dict_list)))) {
        gtk_list_box_remove(data->dict_list, child);
    }

    if (data->settings->dictionaries->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No dictionaries available");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Add a dictionary file or rescan configured directories.");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(data->dict_list, GTK_WIDGET(row));
        refresh_move_buttons(data);
        return;
    }

    for (guint i = 0; i < data->settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(data->settings->dictionaries, i);

        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), cfg->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), cfg->path);
        g_object_set_data_full(G_OBJECT(row), "dict-id", g_strdup(cfg->id), g_free);

        GtkWidget *group_check = gtk_check_button_new();
        gtk_widget_set_valign(group_check, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(group_check, "Select dictionary for group creation");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(group_check),
            g_hash_table_contains(data->group_selection_ids, cfg->id));
        g_object_set_data_full(G_OBJECT(group_check), "dict-id", g_strdup(cfg->id), g_free);
        g_signal_connect(group_check, "toggled", G_CALLBACK(on_group_select_toggled), data);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), group_check);

        GtkWidget *switch_widget = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(switch_widget), cfg->enabled);
        gtk_widget_set_valign(switch_widget, GTK_ALIGN_CENTER);

        DictSwitchData *sd = g_new(DictSwitchData, 1);
        sd->cfg = cfg;
        g_signal_connect_data(switch_widget, "state-set", G_CALLBACK(on_dict_switch_state),
            sd, dict_switch_data_destroy, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), switch_widget);

        GtkWidget *remove_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(remove_btn, "flat");
        gtk_widget_add_css_class(remove_btn, "error");
        gtk_widget_set_valign(remove_btn, GTK_ALIGN_CENTER);

        DictRemoveData *rd = g_new(DictRemoveData, 1);
        rd->data = data;
        rd->id = g_strdup(cfg->id);
        g_signal_connect_data(remove_btn, "clicked", G_CALLBACK(on_remove_dictionary_clicked),
            rd, dict_remove_data_destroy, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_btn);
        gtk_list_box_append(data->dict_list, GTK_WIDGET(row));
    }

    refresh_move_buttons(data);
}

static void update_group_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->group_list)))) {
        gtk_list_box_remove(data->group_list, child);
    }

    if (data->settings->dictionary_groups->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No custom groups");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Tick one or more dictionaries above, then create a group.");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(data->group_list, GTK_WIDGET(row));
        return;
    }

    for (guint i = 0; i < data->settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(data->settings->dictionary_groups, i);

        char subtitle[256];
        snprintf(subtitle, sizeof(subtitle), "%u dictionaries", grp->members->len);

        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), grp->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

        GtkWidget *remove_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(remove_btn, "flat");
        gtk_widget_add_css_class(remove_btn, "error");
        gtk_widget_set_valign(remove_btn, GTK_ALIGN_CENTER);

        GroupRemoveData *rd = g_new(GroupRemoveData, 1);
        rd->data = data;
        rd->id = g_strdup(grp->id);
        g_signal_connect_data(remove_btn, "clicked", G_CALLBACK(on_remove_group_clicked),
            rd, group_remove_data_destroy, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_btn);
        gtk_list_box_append(data->group_list, GTK_WIDGET(row));
    }
}

static void on_dialog_closed(SettingsDialogData *data) {
    settings_save(data->settings);
    if (data->reload_callback) {
        data->reload_callback(data->reload_user_data);
    }
    g_free(data->selected_dict_id);
    if (data->group_selection_ids) {
        g_hash_table_unref(data->group_selection_ids);
    }
    g_free(data);
}

// Theme change callback
static void on_theme_changed(AdwComboRow *row, GParamSpec *pspec, SettingsDialogData *data) {
    (void)pspec;
    guint idx = adw_combo_row_get_selected(row);
    const char *theme_str = (idx == 1) ? "light" : (idx == 2) ? "dark" : "system";
    g_free(data->settings->theme);
    data->settings->theme = g_strdup(theme_str);

    // Apply theme immediately via style manager
    if (data->style_manager) {
        if (idx == 1)
            adw_style_manager_set_color_scheme(data->style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
        else if (idx == 2)
            adw_style_manager_set_color_scheme(data->style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
        else
            adw_style_manager_set_color_scheme(data->style_manager, ADW_COLOR_SCHEME_DEFAULT);
    }
}

// Main dialog creation
GtkWidget* settings_dialog_new(GtkWindow *parent, AppSettings *settings,
                               AdwStyleManager *style_manager,
                               void (*reload_callback)(void *), void *reload_user_data) {
    (void)parent;
    SettingsDialogData *data = g_new0(SettingsDialogData, 1);
    data->settings = settings;
    data->group_selection_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    data->style_manager = style_manager;
    data->reload_callback = reload_callback;
    data->reload_user_data = reload_user_data;

    AdwDialog *dialog = adw_preferences_dialog_new();
    data->dialog = dialog;

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Preferences");
    adw_preferences_page_set_icon_name(page, "folder-dictionaries-symbolic");
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
    
    // Theme group
    AdwPreferencesGroup *theme_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(theme_group, "Appearance");
    adw_preferences_group_set_description(theme_group, "Customize light/dark theme behavior");
    adw_preferences_page_add(page, theme_group);

    AdwComboRow *theme_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme_row), "Theme");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(theme_row), "Choose app color scheme");
    
    GtkStringList *theme_model = gtk_string_list_new((const char *[]){"System", "Light", "Dark", NULL});
    adw_combo_row_set_model(theme_row, G_LIST_MODEL(theme_model));
    g_object_unref(theme_model);
    
    int theme_idx = 0;
    if (settings->theme && strcmp(settings->theme, "light") == 0) theme_idx = 1;
    else if (settings->theme && strcmp(settings->theme, "dark") == 0) theme_idx = 2;
    adw_combo_row_set_selected(theme_row, theme_idx);
    
    g_signal_connect(theme_row, "notify::selected", G_CALLBACK(on_theme_changed), data);
    
    adw_preferences_group_add(theme_group, GTK_WIDGET(theme_row));
    
    // Directory group
    AdwPreferencesGroup *dir_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(dir_group, "Dictionary Directories");
    adw_preferences_group_set_description(dir_group, "Diction scans these directories and auto-adds all supported dictionaries");
    adw_preferences_page_add(page, dir_group);
    
    data->dir_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(data->dir_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(data->dir_list), "boxed-list");
    adw_preferences_group_add(dir_group, GTK_WIDGET(data->dir_list));
    
    GtkWidget *dir_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    
    GtkWidget *add_dir_btn = gtk_button_new_with_label("Add Directory…");
    gtk_widget_add_css_class(add_dir_btn, "suggested-action");
    g_signal_connect(add_dir_btn, "clicked", G_CALLBACK(on_add_directory), data);
    
    GtkWidget *rescan_btn = gtk_button_new_with_label("Rescan");
    g_signal_connect(rescan_btn, "clicked", G_CALLBACK(on_rescan_directories), data);
    
    gtk_box_append(GTK_BOX(dir_box), add_dir_btn);
    gtk_box_append(GTK_BOX(dir_box), rescan_btn);
    adw_preferences_group_add(dir_group, dir_box);
    
    // Dictionary group
    AdwPreferencesGroup *dict_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(dict_group, "Dictionaries");
    adw_preferences_group_set_description(dict_group, "Select a dictionary and use Up/Down to reorder priority");
    adw_preferences_page_add(page, dict_group);
    
    data->dict_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(data->dict_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(data->dict_list), "boxed-list");
    g_signal_connect(data->dict_list, "row-selected", G_CALLBACK(on_dict_row_selected), data);
    adw_preferences_group_add(dict_group, GTK_WIDGET(data->dict_list));
    
    GtkWidget *reorder_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    
    data->move_up_btn = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(data->move_up_btn, "Move selected dictionary up");
    
    DictMoveData *up_data = g_new(DictMoveData, 1);
    up_data->data = data;
    up_data->direction = -1;
    up_data->id = NULL; // id is read from data->selected_dict_id at click time
    g_signal_connect_data(data->move_up_btn, "clicked", G_CALLBACK(on_move_dictionary),
        up_data, dict_move_data_destroy, 0);
    
    data->move_down_btn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(data->move_down_btn, "Move selected dictionary down");
    
    DictMoveData *down_data = g_new(DictMoveData, 1);
    down_data->data = data;
    down_data->direction = 1;
    down_data->id = NULL; // id is read from data->selected_dict_id at click time
    g_signal_connect_data(data->move_down_btn, "clicked", G_CALLBACK(on_move_dictionary),
        down_data, dict_move_data_destroy, 0);
    
    data->create_group_btn = gtk_button_new_with_label("Create Group from Selected…");
    g_signal_connect(data->create_group_btn, "clicked", G_CALLBACK(on_create_group_from_selected), data);
    
    gtk_box_append(GTK_BOX(reorder_box), data->move_up_btn);
    gtk_box_append(GTK_BOX(reorder_box), data->move_down_btn);
    gtk_box_append(GTK_BOX(reorder_box), data->create_group_btn);
    adw_preferences_group_add(dict_group, reorder_box);
    
    GtkWidget *add_file_btn = gtk_button_new_with_label("Add Dictionary File…");
    gtk_widget_add_css_class(add_file_btn, "suggested-action");
    g_signal_connect(add_file_btn, "clicked", G_CALLBACK(on_add_dictionary_file), data);
    adw_preferences_group_add(dict_group, add_file_btn);
    
    // Groups group
    AdwPreferencesGroup *group_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group_group, "Dictionary Groups");
    adw_preferences_group_set_description(group_group, "Custom groups appear in the sidebar Groups tab");
    adw_preferences_page_add(page, group_group);
    
    data->group_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(data->group_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(data->group_list), "boxed-list");
    adw_preferences_group_add(group_group, GTK_WIDGET(data->group_list));
    
    // Initial population
    update_dir_list(data);
    update_dict_list(data);
    update_group_list(data);
    refresh_move_buttons(data);
    
    // Close handler
    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(on_dialog_closed), data);
    
    return GTK_WIDGET(dialog);
}

void settings_dialog_run(GtkWidget *dialog) {
    (void)dialog;
}
