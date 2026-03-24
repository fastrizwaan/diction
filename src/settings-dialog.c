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

static void on_add_directory_response(GtkNativeDialog *dialog, int response, SettingsDialogData *data) {
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        char *path = g_file_get_path(file);
        if (path) {
            settings_add_directory(data->settings, path);
            update_dir_list(data);
            g_free(path);
        }
        g_object_unref(file);
    }
    g_object_unref(dialog);
}

static void on_add_directory(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    GtkFileDialog *chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, "Select Dictionary Directory");
    gtk_file_dialog_select_folder(chooser, GTK_WINDOW(data->dialog), NULL,
        (GAsyncReadyCallback)on_add_directory_response, data);
}

static void on_remove_directory_clicked(GtkButton *btn, DirRemoveData *d) {
    settings_remove_directory(d->data->settings, d->path);
    update_dir_list(d->data);
}

static void on_rescan_directories(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    settings_save(data->settings);
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

static void on_move_dictionary(GtkButton *btn, DictMoveData *d) {
    (void)btn;
    settings_move_dictionary(d->data->settings, d->id, d->direction);
    update_dict_list(d->data);
    refresh_move_buttons(d->data);
}

static void on_add_dictionary_file_response(GtkNativeDialog *dialog, int response, SettingsDialogData *data) {
    if (response == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
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
    }
    g_object_unref(dialog);
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
        (GAsyncReadyCallback)on_add_dictionary_file_response, data);
}

typedef struct {
    SettingsDialogData *data;
    char *id;
} DictRemoveData;

static void dict_remove_data_free(DictRemoveData *d) {
    g_free(d->id);
    g_free(d);
}

static void on_remove_dictionary_clicked(GtkButton *btn, DictRemoveData *d) {
    (void)btn;
    settings_remove_dictionary(d->data->settings, d->id);
    g_free(d->data->selected_dict_id);
    d->data->selected_dict_id = NULL;
    update_dict_list(d->data);
    refresh_move_buttons(d->data);
}

typedef struct {
    SettingsDialogData *data;
    DictConfig *cfg;
} DictSwitchData;

static void dict_switch_data_free(DictSwitchData *d) {
    g_free(d);
}

static void on_dict_switch_state(GtkSwitch *sw, GVariant *state, DictConfig *cfg) {
    (void)sw;
    cfg->enabled = g_variant_get_boolean(state);
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

static void refresh_move_buttons(SettingsDialogData *data) {
    int has_selection = (data->selected_dict_id != NULL);
    int can_move_up = 0, can_move_down = 0;
    
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
    gtk_widget_set_sensitive(data->create_group_btn, has_selection);
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

static void on_remove_group_clicked(GtkButton *btn, GroupRemoveData *d) {
    (void)btn;
    settings_remove_group(d->data->settings, d->id);
    update_group_list(d->data);
}

static void on_create_group_response(AdwAlertDialog *dialog, const char *response, GtkEntry *entry) {
    SettingsDialogData *data = g_object_get_data(G_OBJECT(dialog), "settings-data");

    if (strcmp(response, "create") == 0) {
        const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (name && strlen(name) > 0) {
            GPtrArray *ids = g_ptr_array_new();
            g_ptr_array_add(ids, (gpointer)data->selected_dict_id);
            settings_create_group(data->settings, name, ids);
            g_ptr_array_free(ids, FALSE);
            update_group_list(data);
        }
    }
}

static void on_create_group_from_selected(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    if (!data->selected_dict_id) return;

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
            rd, (GClosureNotify)dir_remove_data_free, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_btn);
        gtk_list_box_append(data->dir_list, GTK_WIDGET(row));
    }
}

static void update_dict_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->dict_list)))) {
        gtk_list_box_remove(data->dict_list, child);
    }

    for (guint i = 0; i < data->settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(data->settings->dictionaries, i);

        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), cfg->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), cfg->path);
        g_object_set_data_full(G_OBJECT(row), "dict-id", g_strdup(cfg->id), g_free);

        GtkWidget *switch_widget = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(switch_widget), cfg->enabled);
        gtk_widget_set_valign(switch_widget, GTK_ALIGN_CENTER);

        DictSwitchData *sd = g_new(DictSwitchData, 1);
        sd->cfg = cfg;
        g_signal_connect_data(switch_widget, "state-set", G_CALLBACK(on_dict_switch_state),
            sd, (GClosureNotify)dict_switch_data_free, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), switch_widget);

        GtkWidget *remove_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(remove_btn, "flat");
        gtk_widget_add_css_class(remove_btn, "error");
        gtk_widget_set_valign(remove_btn, GTK_ALIGN_CENTER);

        DictRemoveData *rd = g_new(DictRemoveData, 1);
        rd->data = data;
        rd->id = g_strdup(cfg->id);
        g_signal_connect_data(remove_btn, "clicked", G_CALLBACK(on_remove_dictionary_clicked),
            rd, (GClosureNotify)dict_remove_data_free, 0);

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
            rd, (GClosureNotify)group_remove_data_free, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_btn);
        gtk_list_box_append(data->group_list, GTK_WIDGET(row));
    }
}

static void on_dialog_closed(SettingsDialogData *data) {
    settings_save(data->settings);
    g_free(data->selected_dict_id);
    g_free(data);
}

// Theme change callback
static void on_theme_changed(AdwComboRow *row, GParamSpec *pspec, SettingsDialogData *data) {
    (void)pspec;
    guint idx = adw_combo_row_get_selected(row);
    char *new_theme = g_strdup(idx == 1 ? "light" : (idx == 2 ? "dark" : "system"));
    g_free(data->settings->theme);
    data->settings->theme = new_theme;
}

// Main dialog creation
GtkWidget* settings_dialog_new(GtkWindow *parent, AppSettings *settings) {
    SettingsDialogData *data = g_new0(SettingsDialogData, 1);
    data->settings = settings;

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
    up_data->id = NULL;
    g_signal_connect_data(data->move_up_btn, "clicked", G_CALLBACK(on_move_dictionary),
        up_data, (GClosureNotify)dict_move_data_free, 0);
    
    data->move_down_btn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(data->move_down_btn, "Move selected dictionary down");
    
    DictMoveData *down_data = g_new(DictMoveData, 1);
    down_data->data = data;
    down_data->direction = 1;
    down_data->id = NULL;
    g_signal_connect_data(data->move_down_btn, "clicked", G_CALLBACK(on_move_dictionary),
        down_data, (GClosureNotify)dict_move_data_free, 0);
    
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
