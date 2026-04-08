#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <webkit/webkit.h>
#include <pango/pangocairo.h>

/* Normalize path strings used as keys in the scanning UI. This ensures the
 * same canonical form is used when creating rows and when progress updates
 * are reported from extraction code. Returns a newly-allocated string. */
static char *normalize_scan_path(const char *path) {
    if (!path) return g_strdup("");
    char *valid = g_utf8_make_valid(path, -1);
    char *canon = g_canonicalize_filename(valid, NULL);
    g_free(valid);
    if (!canon) return g_strdup("");
    return canon;
}

typedef struct _SettingsDialogData {
    AdwDialog          *dialog;
    AppSettings        *settings;
    GtkListBox         *dir_list;
    GtkListBox         *dict_list;
    GtkListBox         *group_list;
    GtkWidget          *move_up_btn;
    GtkWidget          *move_down_btn;
    GtkWidget          *create_group_btn;
    char               *selected_dict_id;
    GHashTable         *group_selection_ids;
    AdwStyleManager    *style_manager;
    GtkWindow          *parent_window; /* Parent window for file dialogs */
    void (*reload_callback)(void *);
    void (*soft_reload_callback)(void *);
    void *user_data;
    /* font change sink */
    void (*font_changed_callback)(void *);
    void *font_changed_user_data;
} SettingsDialogData;

/* Forward declarations */
static void update_dir_list(SettingsDialogData *data);
static void update_dict_list(SettingsDialogData *data);
static void update_group_list(SettingsDialogData *data);
static void refresh_move_buttons(SettingsDialogData *data);


/* ---- Directory callbacks ---- */

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
            /* Add to configured directories and show a scanning dialog that
             * lists discovered dictionaries and shows progress. After the
             * scan completes we trigger a full reload. */
            settings_add_directory(data->settings, path);
            update_dir_list(data);
            /* Prepare single-entry dir array for scanner helper */
            char **dirs = g_new0(char *, 2);
            dirs[0] = g_strdup(path);
            dirs[1] = NULL;
            extern void show_scan_dialog_for_dirs(SettingsDialogData *data, char **dirs, int n_dirs, gboolean call_reload);
            show_scan_dialog_for_dirs(data, dirs, 1, TRUE);
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
    gtk_file_dialog_select_folder(chooser, data->parent_window, NULL,
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
    if (d->data->reload_callback) d->data->reload_callback(d->data->user_data);
    g_idle_add(on_remove_directory_idle, d->data);
}

static void on_rescan_directories(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    /* Save settings and show a scanning dialog for all configured
     * directories. When the scan finishes we trigger a full reload. */
    settings_save(data->settings);
    if (data->settings->dictionary_dirs->len == 0) {
        if (data->reload_callback)
            data->reload_callback(data->user_data);
        return;
    }
    int n = (int)data->settings->dictionary_dirs->len;
    char **dirs = g_new0(char *, n + 1);
    for (int i = 0; i < n; i++)
        dirs[i] = g_strdup(g_ptr_array_index(data->settings->dictionary_dirs, i));
    dirs[n] = NULL;
    extern void show_scan_dialog_for_dirs(SettingsDialogData *data, char **dirs, int n_dirs, gboolean call_reload);
    show_scan_dialog_for_dirs(data, dirs, n, TRUE);
}

/* ---- Scanning dialog implementation ---- */

typedef struct {
    SettingsDialogData *data;
    AdwDialog *dialog;
    GtkListBox *list;
    GtkWidget *status_label;
    GtkWidget *spinner;
    GtkWidget *cancel_btn;
    GtkWidget *close_btn;
    volatile gint generation; /* used for cancellation */
    int found_count;
    int total_dirs;
    gboolean call_reload;
    /* When integrating with main loader, only show entries from these dirs */
    char **scan_dirs;
    int n_scan_dirs;
    GHashTable *row_map; /* map path -> GtkWidget* row for progress updates */
} ScanContext;

typedef struct {
    ScanContext *ctx;
    char *name;
    char *path;
    gboolean done;
    gboolean is_progress_update;
    int progress;
} ScanIdleData;

typedef struct {
    ScanContext *ctx;
    char **dirs;
    int n_dirs;
} ScanThreadArgs;

/* Active scan contexts registered when integrating with the main loader */
static GList *active_scan_contexts = NULL;

static gboolean scan_idle_add_entry(gpointer user_data) {
    ScanIdleData *sid = user_data;
    ScanContext *ctx = sid->ctx;

    /* Progress update for an existing row */
    if (sid->is_progress_update) {
        if (sid->path && ctx->row_map) {
            gpointer val = g_hash_table_lookup(ctx->row_map, sid->path);
            if (val) {
                GtkWidget *row = val;
                char buf[128];
                g_snprintf(buf, sizeof(buf), "Extracting %d%%", sid->progress);
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), buf);
            }
        }
    } else if (!sid->done) {
        /* If a prelisted row exists for this path, update it instead of
         * creating a duplicate entry. */
        if (sid->path && ctx->row_map) {
            gpointer existing = g_hash_table_lookup(ctx->row_map, sid->path);
            if (existing) {
                GtkWidget *row = existing;
                if (sid->name && *sid->name) {
                    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sid->name);
                }
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sid->path);
                g_free(sid->name);
                g_free(sid->path);
                g_free(sid);
                return G_SOURCE_REMOVE;
            }
        }

        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sid->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sid->path);
        gtk_list_box_append(ctx->list, GTK_WIDGET(row));
        /* store mapping path -> row for progress updates */
        if (sid->path) {
            if (!ctx->row_map) ctx->row_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            g_hash_table_replace(ctx->row_map, g_strdup(sid->path), row);
        }
        ctx->found_count++;
        char buf[128];
        g_snprintf(buf, sizeof(buf), "%d dictionaries found", ctx->found_count);
        gtk_label_set_text(GTK_LABEL(ctx->status_label), buf);
    } else {
        /* Scan finished */
        gtk_spinner_stop(GTK_SPINNER(ctx->spinner));
        char buf[256];
        g_snprintf(buf, sizeof(buf), "Scan complete — %d dictionaries found", ctx->found_count);
        gtk_label_set_text(GTK_LABEL(ctx->status_label), buf);
        if (ctx->close_btn) gtk_widget_set_sensitive(ctx->close_btn, TRUE);
        if (ctx->cancel_btn) gtk_widget_set_sensitive(ctx->cancel_btn, FALSE);
        if (!ctx->call_reload && ctx->data && ctx->data->reload_callback) ctx->data->reload_callback(ctx->data->user_data);
    }

    g_free(sid->name);
    g_free(sid->path);
    g_free(sid);
    return G_SOURCE_REMOVE;
}

static void scan_worker_callback(DictEntry *entry, void *user_data) {
    ScanContext *ctx = user_data;
    if (!entry) return;

    /* Copy name/path for main-thread UI update */
    char *name_copy = entry->name ? g_strdup(entry->name) : g_strdup("(Unknown)");
    /* Use canonicalized path so progress updates match the stored key */
    char *path_copy = normalize_scan_path(entry->path);

    ScanIdleData *sid = g_new0(ScanIdleData, 1);
    sid->ctx = ctx;
    sid->name = name_copy;
    sid->path = path_copy;
    sid->done = FALSE;
    g_idle_add(scan_idle_add_entry, sid);

    /* Free the loaded entry (we don't keep the mmap in settings dialog) */
    dict_loader_free(entry);
}

static gpointer scan_thread_func(gpointer user_data) {
    ScanThreadArgs *args = user_data;
    ScanContext *ctx = args->ctx;

    /* Initialize generation value used for cancellation checks */
    ctx->generation = 1;
    gint expected = ctx->generation;

    for (int i = 0; i < args->n_dirs; i++) {
        if (g_atomic_int_get(&ctx->generation) != expected) break;
        /* Update status for current directory */
        char status_buf[256];
        g_snprintf(status_buf, sizeof(status_buf), "Scanning %d of %d:\n%s",
                   i + 1, args->n_dirs, args->dirs[i]);
        /* Update status label on main thread */
        ScanIdleData *st = g_new0(ScanIdleData, 1);
        st->ctx = ctx;
        st->name = g_strdup(status_buf);
        st->path = g_strdup("");
        st->done = FALSE;
        g_idle_add(scan_idle_add_entry, st);

        dict_loader_scan_directory_streaming(args->dirs[i], scan_worker_callback, ctx, &ctx->generation, expected);
    }

    /* Signal completion */
    ScanIdleData *done = g_new0(ScanIdleData, 1);
    done->ctx = ctx;
    done->name = g_strdup("done");
    done->path = g_strdup("");
    done->done = TRUE;
    g_idle_add(scan_idle_add_entry, done);

    /* Free thread args */
    for (int i = 0; i < args->n_dirs; i++)
        g_free(args->dirs[i]);
    g_free(args->dirs);
    g_free(args);
    return NULL;
}

static void on_scan_cancel_clicked(GtkButton *btn, ScanContext *ctx) {
    (void)btn;
    /* Increment generation to request cancellation */
    if (ctx->call_reload) {
        /* Integrated with main loader: request loader cancellation */
        extern void request_loader_cancel(void);
        request_loader_cancel();
    } else {
        /* Local scan: bump the context generation to stop thread loop */
        g_atomic_int_inc(&ctx->generation);
    }
    if (ctx->cancel_btn) gtk_widget_set_sensitive(ctx->cancel_btn, FALSE);
    if (ctx->status_label) gtk_label_set_text(GTK_LABEL(ctx->status_label), "Canceling…");
}

static void on_scan_close_clicked(GtkButton *btn, ScanContext *ctx) {
    (void)btn;
    if (ctx->dialog) adw_dialog_close(ctx->dialog);
}

static void on_scan_dialog_closed(ScanContext *ctx) {
    /* Remove from global registry and free context-owned resources. */
    active_scan_contexts = g_list_remove(active_scan_contexts, ctx);
    if (ctx->scan_dirs) {
        for (int i = 0; i < ctx->n_scan_dirs; i++) g_free(ctx->scan_dirs[i]);
        g_free(ctx->scan_dirs);
    }
    if (ctx->row_map) g_hash_table_destroy(ctx->row_map);
    g_free(ctx);
}

/* Called by main loader to notify UI of discovered dictionaries. */
void settings_scan_notify(const char *name, const char *path, gboolean done) {
    if (!active_scan_contexts) return;
    /* Debug: log incoming notifications to help diagnose missing titles */
    g_printerr("[SETTINGS SCAN NOTIFY] name='%s' path='%s' done=%d\n",
                name ? name : "(null)", path ? path : "(null)", done ? 1 : 0);
    /* Precompute a canonical path used for matching / row keys */
    char *canonical_path = normalize_scan_path(path);
    for (GList *l = active_scan_contexts; l; l = l->next) {
        ScanContext *ctx = l->data;
        /* Only notify contexts that requested integration */
        if (!ctx->call_reload) continue;

        /* If this context has a dir filter, ensure canonical path matches one of them */
        gboolean match = FALSE;
        if (done) match = TRUE; /* always notify completion */
        else if (!canonical_path || *canonical_path == '\0') match = FALSE;
        else if (!ctx->scan_dirs || ctx->n_scan_dirs == 0) match = TRUE;
        else {
            for (int i = 0; i < ctx->n_scan_dirs; i++) {
                if (ctx->scan_dirs[i] && g_str_has_prefix(canonical_path, ctx->scan_dirs[i])) { match = TRUE; break; }
            }
        }
        if (!match) continue;

        ScanIdleData *sid = g_new0(ScanIdleData, 1);
        sid->ctx = ctx;
        sid->name = name ? g_strdup(name) : g_strdup("");
        /* Use the precomputed canonical path as the row key */
        sid->path = g_strdup(canonical_path);
        sid->done = done ? TRUE : FALSE;
        g_idle_add(scan_idle_add_entry, sid);
    }
    g_free(canonical_path);
}

/* Called by extraction code to provide progress updates for a specific
 * dictionary path. Percent should be 0..100. This posts an idle to update
 * the UI row subtitle for the matching path. */
void settings_scan_progress_notify(const char *path, int percent) {
    if (!active_scan_contexts) return;
    /* Precompute canonical path for matching and row lookup */
    char *canonical_path = normalize_scan_path(path);
    for (GList *l = active_scan_contexts; l; l = l->next) {
        ScanContext *ctx = l->data;
        if (!ctx->call_reload) continue;

        /* If this context has a dir filter, ensure canonical path matches one of them */
        gboolean match = FALSE;
        if (!canonical_path || *canonical_path == '\0') match = FALSE;
        else if (!ctx->scan_dirs || ctx->n_scan_dirs == 0) match = TRUE;
        else {
            for (int i = 0; i < ctx->n_scan_dirs; i++) {
                if (ctx->scan_dirs[i] && g_str_has_prefix(canonical_path, ctx->scan_dirs[i])) { match = TRUE; break; }
            }
        }
        if (!match) continue;

        ScanIdleData *sid = g_new0(ScanIdleData, 1);
        sid->ctx = ctx;
        sid->name = g_strdup("");
        sid->path = g_strdup(canonical_path);
        sid->done = FALSE;
        sid->is_progress_update = TRUE;
        sid->progress = percent;
        g_idle_add(scan_idle_add_entry, sid);
    }
    g_free(canonical_path);
}

/* Public helper used by handlers above */
void show_scan_dialog_for_dirs(SettingsDialogData *data, char **dirs, int n_dirs, gboolean call_reload) {
    /* Build dialog */
    AdwDialog *dialog = adw_alert_dialog_new("Scanning Directories", NULL);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog), "Scanning directories for dictionaries…");

    GtkWidget *extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(extra), spinner);

    GtkWidget *status = gtk_label_new("Starting scan...");
    gtk_box_append(GTK_BOX(extra), status);

    GtkListBox *list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(list), "boxed-list");
    gtk_widget_set_size_request(GTK_WIDGET(list), 600, 240);
    gtk_box_append(GTK_BOX(extra), GTK_WIDGET(list));

    /* Buttons area inside extra child */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_sensitive(close_btn, FALSE);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), close_btn);
    gtk_box_append(GTK_BOX(extra), btn_box);

    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), extra);

    /* Create context */
    ScanContext *ctx = g_new0(ScanContext, 1);
    ctx->data = data;
    ctx->dialog = dialog;
    ctx->list = list;
    ctx->status_label = status;
    ctx->spinner = spinner;
    ctx->cancel_btn = cancel_btn;
    ctx->close_btn = close_btn;
    ctx->found_count = 0;
    ctx->total_dirs = n_dirs;
    ctx->call_reload = call_reload;

    /* Pre-list supported files in the requested directories so the user sees
     * everything that will be processed before extraction begins. */
    if (dirs && n_dirs > 0) {
        /* Helper: check suffixes */
        for (int di = 0; di < n_dirs; di++) {
            const char *dpath = dirs[di];
            if (!dpath) continue;
            if (!g_file_test(dpath, G_FILE_TEST_IS_DIR)) continue;
            GDir *gd = g_dir_open(dpath, 0, NULL);
            if (!gd) continue;
            const char *ename;
            while ((ename = g_dir_read_name(gd)) != NULL) {
                if (ename[0] == '.') continue;
                /* Case-insensitive suffix check */
                char *low = g_ascii_strdown(ename, -1);
                gboolean ok = FALSE;
                if (g_str_has_suffix(low, ".dsl") || g_str_has_suffix(low, ".dsl.dz") ||
                    g_str_has_suffix(low, ".mdx") || g_str_has_suffix(low, ".ifo") ||
                    g_str_has_suffix(low, ".bgl")) ok = TRUE;
                g_free(low);
                if (!ok) continue;

                char *full = g_build_filename(dpath, ename, NULL);
                char *canon = normalize_scan_path(full);
                if (!canon) { g_free(full); continue; }

                /* Create a pending row if not already present */
                if (!ctx->row_map) ctx->row_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                if (!g_hash_table_lookup(ctx->row_map, canon)) {
                    GtkWidget *row = adw_action_row_new();
                    char *b = g_path_get_basename(full);
                    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), b);
                    g_free(b);
                    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), canon);
                    gtk_list_box_append(ctx->list, GTK_WIDGET(row));
                    g_hash_table_replace(ctx->row_map, g_strdup(canon), row);
                    ctx->found_count++;
                }

                g_free(full);
                g_free(canon);
            }
            g_dir_close(gd);
        }
        char buf[128];
        g_snprintf(buf, sizeof(buf), "%d dictionaries listed", ctx->found_count);
        gtk_label_set_text(GTK_LABEL(ctx->status_label), buf);
    }

    /* Wire buttons */
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_scan_cancel_clicked), ctx);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_scan_close_clicked), ctx);

    /* Present dialog and either integrate with the main loader or
     * perform a local scan depending on `call_reload`. */
    if (call_reload && data && data->reload_callback) {
        /* Copy requested dirs into context for filtering notifications (canonicalized) */
        ctx->scan_dirs = g_new0(char *, n_dirs + 1);
        for (int i = 0; i < n_dirs; i++) ctx->scan_dirs[i] = normalize_scan_path(dirs[i]);
        ctx->n_scan_dirs = n_dirs;

        /* Register context so main loader can notify it */
        active_scan_contexts = g_list_prepend(active_scan_contexts, ctx);

        /* Ensure cleanup when dialog closes */
        g_signal_connect_swapped(dialog, "closed", G_CALLBACK(on_scan_dialog_closed), ctx);

        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(data->dialog));

        /* Kick off the main loader (will call back via settings_scan_notify) */
        data->reload_callback(data->user_data);

        /* Free the dirs array passed in by caller (we duplicated the strings into ctx) */
        for (int i = 0; i < n_dirs; i++) g_free(dirs[i]);
        g_free(dirs);
        return;
    }

    /* Present dialog for local scanning (not integrated). */
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(data->dialog));

    /* Start scanning thread */
    ScanThreadArgs *args = g_new0(ScanThreadArgs, 1);
    args->ctx = ctx;
    args->n_dirs = n_dirs;
    args->dirs = g_new0(char *, n_dirs + 1);
    for (int i = 0; i < n_dirs; i++)
        args->dirs[i] = g_strdup(dirs[i]);
    args->dirs[n_dirs] = NULL;

    GThread *t = g_thread_new("settings-scan", scan_thread_func, args);
    g_thread_unref(t);

    /* Free the dirs array passed in by caller (we duplicated the strings) */
    for (int i = 0; i < n_dirs; i++) g_free(dirs[i]);
    g_free(dirs);
}

/* ---- Dictionary callbacks ---- */

typedef struct {
    SettingsDialogData *data;
    char *id;
    int direction;
} DictMoveData;

/* Per-row import button data */
typedef struct {
    SettingsDialogData *data;
    char *path; /* source path to import */
} ImportRowData;

static void import_row_data_free(ImportRowData *d) {
    if (!d) return;
    g_free(d->path);
    g_free(d);
}

static void import_row_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    import_row_data_free(data);
}

static void on_import_row_clicked(GtkButton *btn, ImportRowData *d) {
    (void)btn;
    if (!d || !d->path || !d->data) return;
    if (settings_import_dictionary(d->data->settings, d->path)) {
        update_dict_list(d->data);
        if (d->data->reload_callback) d->data->reload_callback(d->data->user_data);
    } else {
        g_printerr("Import failed for %s\n", d->path);
    }
}

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
    const char *id = d->data->selected_dict_id;
    if (!id) return;
    settings_move_dictionary(d->data->settings, id, d->direction);
    if (d->data->soft_reload_callback) d->data->soft_reload_callback(d->data->user_data);
    update_dict_list(d->data);
    refresh_move_buttons(d->data);
}

static void on_add_dictionary_file_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *chooser = GTK_FILE_DIALOG(source);
    SettingsDialogData *data = user_data;
    GError *error = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(chooser, result, &error);
    if (files) {
        guint n = g_list_model_get_n_items(files);
        for (guint i = 0; i < n; i++) {
            GObject *obj = g_list_model_get_item(files, i);
            GFile *file = G_FILE(obj);
            char *path = g_file_get_path(file);
            if (path) {
                char *name = g_path_get_basename(path);
                char *ext = strrchr(name, '.');
                if (ext) *ext = '\0';
                settings_add_dictionary(data->settings, name, path);
                g_free(name);
                g_free(path);
            }
            g_object_unref(obj);
        }
        g_object_unref(files);
        update_dict_list(data);
        if (data->reload_callback) data->reload_callback(data->user_data);
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

    gtk_file_dialog_open_multiple(chooser, data->parent_window, NULL,
        on_add_dictionary_file_response, data);
}

static void on_import_dictionary_files_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *chooser = GTK_FILE_DIALOG(source);
    SettingsDialogData *data = user_data;
    GError *error = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(chooser, result, &error);
    if (files) {
        guint n = g_list_model_get_n_items(files);
        for (guint i = 0; i < n; i++) {
            GObject *obj = g_list_model_get_item(files, i);
            GFile *file = G_FILE(obj);
            char *path = g_file_get_path(file);
            if (path) {
                /* Import (copy) into app data and add to settings */
                if (!settings_import_dictionary(data->settings, path)) {
                    g_printerr("Failed to import: %s\n", path);
                }
                g_free(path);
            }
            g_object_unref(obj);
        }
        g_object_unref(files);
        update_dict_list(data);
        if (data->reload_callback) data->reload_callback(data->user_data);
    } else if (error) {
        g_error_free(error);
    }
    g_object_unref(chooser);
}

static void on_import_dictionary_files(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    GtkFileDialog *chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, "Import Dictionary Files");

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

    gtk_file_dialog_open_multiple(chooser, data->parent_window, NULL,
        on_import_dictionary_files_response, data);
}

/* Remove dictionary — holds only id string, not a raw cfg* pointer */
typedef struct {
    SettingsDialogData *data;
    char *id;  /* heap-copy — safe after list rebuild */
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
    /* Save id before removal because settings_remove_dictionary frees cfg */
    char *id_copy = g_strdup(d->id);
    settings_remove_dictionary(d->data->settings, id_copy);
    g_hash_table_remove(d->data->group_selection_ids, id_copy);
    if (d->data->selected_dict_id &&
        strcmp(d->data->selected_dict_id, id_copy) == 0) {
        g_free(d->data->selected_dict_id);
        d->data->selected_dict_id = NULL;
    }
    g_free(id_copy);
    /* Save immediately so a re-scan doesn't re-add it */
    settings_save(d->data->settings);
    if (d->data->soft_reload_callback) d->data->soft_reload_callback(d->data->user_data);
    g_idle_add(on_remove_dictionary_idle, d->data);
}

/* Switch — holds only cfg pointer; we save settings immediately on toggle */
typedef struct {
    SettingsDialogData *data;
    char *dict_id; /* heap-copy of id — look up cfg at signal time */
} DictSwitchData;

static void dict_switch_data_free(DictSwitchData *d) {
    g_free(d->dict_id);
    g_free(d);
}
static void dict_switch_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    dict_switch_data_free(data);
}

static gboolean on_dict_switch_state(GtkSwitch *sw, gboolean state, DictSwitchData *sd) {
    (void)sw;
    /* Look up cfg by id — safe even after list rebuilds */
    DictConfig *cfg = settings_find_dictionary_by_id(sd->data->settings, sd->dict_id);
    if (cfg) {
        cfg->enabled = state ? 1 : 0;
        /* Persist immediately so restart preserves the state */
        settings_save(sd->data->settings);
        if (sd->data->soft_reload_callback) sd->data->soft_reload_callback(sd->data->user_data);
    }
    return FALSE; /* let GtkSwitch update its visual state */
}

static void on_dict_row_selected(GtkListBox *list, GtkListBoxRow *row, SettingsDialogData *data) {
    (void)list;
    g_free(data->selected_dict_id);
    data->selected_dict_id = NULL;

    if (row) {
        const char *id = g_object_get_data(G_OBJECT(row), "dict-id");
        if (id)
            data->selected_dict_id = g_strdup(id);
    }
    refresh_move_buttons(data);
}

static void on_group_select_toggled(GtkCheckButton *btn, gpointer user_data) {
    SettingsDialogData *data = user_data;
    const char *dict_id = g_object_get_data(G_OBJECT(btn), "dict-id");
    if (!dict_id) return;

    if (gtk_check_button_get_active(btn))
        g_hash_table_add(data->group_selection_ids, g_strdup(dict_id));
    else
        g_hash_table_remove(data->group_selection_ids, dict_id);

    refresh_move_buttons(data);
}

static void refresh_move_buttons(SettingsDialogData *data) {
    int has_selection = (data->selected_dict_id != NULL);
    int can_move_up = 0, can_move_down = 0;
    int has_group_selection = data->group_selection_ids &&
                              g_hash_table_size(data->group_selection_ids) > 0;

    if (has_selection) {
        for (guint i = 0; i < data->settings->dictionaries->len; i++) {
            DictConfig *cfg = g_ptr_array_index(data->settings->dictionaries, i);
            if (strcmp(cfg->id, data->selected_dict_id) == 0) {
                can_move_up   = (i > 0);
                can_move_down = (i < data->settings->dictionaries->len - 1);
                break;
            }
        }
    }

    gtk_widget_set_sensitive(data->move_up_btn,     has_selection && can_move_up);
    gtk_widget_set_sensitive(data->move_down_btn,   has_selection && can_move_down);
    gtk_widget_set_sensitive(data->create_group_btn, has_group_selection);
}

/* ---- Group callbacks ---- */

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
            while (g_hash_table_iter_next(&iter, &key, NULL))
                g_ptr_array_add(ids, key);
            settings_create_group(data->settings, name, ids);
            g_ptr_array_free(ids, FALSE);
            g_hash_table_remove_all(data->group_selection_ids);
            if (data->soft_reload_callback) data->soft_reload_callback(data->user_data);
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

/* ---- UI update functions ---- */

static void update_dir_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->dir_list))))
        gtk_list_box_remove(data->dir_list, child);

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
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->dict_list))))
        gtk_list_box_remove(data->dict_list, child);

    if (data->settings->dictionaries->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No dictionaries available");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
            "Add a dictionary file or rescan configured directories.");
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
        /* Store id as a key for later row identification */
        g_object_set_data_full(G_OBJECT(row), "dict-id", g_strdup(cfg->id), g_free);

        /* Group-selection checkbox */
        GtkWidget *group_check = gtk_check_button_new();
        gtk_widget_set_valign(group_check, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(group_check, "Select for group creation");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(group_check),
            g_hash_table_contains(data->group_selection_ids, cfg->id));
        g_object_set_data_full(G_OBJECT(group_check), "dict-id", g_strdup(cfg->id), g_free);
        g_signal_connect(group_check, "toggled", G_CALLBACK(on_group_select_toggled), data);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), group_check);

        /* Enable/disable switch — use id string, not raw cfg* */
        GtkWidget *switch_widget = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(switch_widget), cfg->enabled);
        gtk_widget_set_valign(switch_widget, GTK_ALIGN_CENTER);

        DictSwitchData *sd = g_new(DictSwitchData, 1);
        sd->data    = data;
        sd->dict_id = g_strdup(cfg->id);
        g_signal_connect_data(switch_widget, "state-set", G_CALLBACK(on_dict_switch_state),
            sd, dict_switch_data_destroy, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), switch_widget);

        /* Remove button — use id string, not raw cfg* */
        GtkWidget *remove_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(remove_btn, "flat");
        gtk_widget_add_css_class(remove_btn, "error");
        gtk_widget_set_valign(remove_btn, GTK_ALIGN_CENTER);

        /* Import button for per-row import into app data (if applicable) */
        GtkWidget *import_btn = gtk_button_new_with_label("Import");
        gtk_widget_add_css_class(import_btn, "flat");
        gtk_widget_set_valign(import_btn, GTK_ALIGN_CENTER);
        /* Disable import for dictionaries that are already manual/managed */
        const char *data_root = g_build_filename(g_get_user_data_dir(), "diction", "dicts", NULL);
        gboolean already_managed = cfg->path && g_str_has_prefix(cfg->path, data_root);
        g_free((gpointer)data_root);
        if (already_managed) gtk_widget_set_sensitive(import_btn, FALSE);

        ImportRowData *ird = g_new0(ImportRowData, 1);
        ird->data = data;
        ird->path = g_strdup(cfg->path);
        g_signal_connect_data(import_btn, "clicked", G_CALLBACK(on_import_row_clicked), ird, import_row_data_destroy, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), import_btn);

        DictRemoveData *rd = g_new(DictRemoveData, 1);
        rd->data = data;
        rd->id   = g_strdup(cfg->id);
        g_signal_connect_data(remove_btn, "clicked", G_CALLBACK(on_remove_dictionary_clicked),
            rd, dict_remove_data_destroy, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_btn);

        gtk_list_box_append(data->dict_list, GTK_WIDGET(row));
    }

    refresh_move_buttons(data);
}

static void update_group_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->group_list))))
        gtk_list_box_remove(data->group_list, child);

    if (data->settings->dictionary_groups->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No custom groups");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
            "Tick one or more dictionaries above, then create a group.");
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
        rd->id   = g_strdup(grp->id);
        g_signal_connect_data(remove_btn, "clicked", G_CALLBACK(on_remove_group_clicked),
            rd, group_remove_data_destroy, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_btn);
        gtk_list_box_append(data->group_list, GTK_WIDGET(row));
    }
}

static void on_theme_row_changed(AdwComboRow *r, GParamSpec *p, SettingsDialogData *d) {
    (void)p;
    guint i = adw_combo_row_get_selected(r);
    const char *ts = (i == 1) ? "light" : (i == 2) ? "dark" : "system";
    g_free(d->settings->theme);
    d->settings->theme = g_strdup(ts);
    settings_save(d->settings);
    if (d->style_manager) {
        if (i == 1) adw_style_manager_set_color_scheme(d->style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
        else if (i == 2) adw_style_manager_set_color_scheme(d->style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
        else adw_style_manager_set_color_scheme(d->style_manager, ADW_COLOR_SCHEME_DEFAULT);
    }
}

static void on_color_theme_row_changed(AdwComboRow *row, GParamSpec *pspec, SettingsDialogData *data) {
    (void)pspec;
    const char *themes[] = {"default", "solarized", "dracula", "nord", "gruvbox", "monokai", "material", "ocean", "forest", "sepia"};
    guint idx = adw_combo_row_get_selected(row);
    if (idx >= G_N_ELEMENTS(themes)) return;
    
    g_free(data->settings->color_theme);
    data->settings->color_theme = g_strdup(themes[idx]);
    settings_save(data->settings);
    
    /* Trigger refresh of results to see new theme */
    if (data->font_changed_callback)
        data->font_changed_callback(data->font_changed_user_data);
}

/* ---- Font callbacks ---- */

static void on_font_family_row_changed(AdwComboRow *row, GParamSpec *pspec, SettingsDialogData *data) {
    (void)pspec;
    GtkStringList *model = GTK_STRING_LIST(adw_combo_row_get_model(row));
    guint idx = adw_combo_row_get_selected(row);
    const char *text = gtk_string_list_get_string(model, idx);
    
    g_free(data->settings->font_family);
    data->settings->font_family = g_strdup(text);
    settings_save(data->settings);
    if (data->font_changed_callback)
        data->font_changed_callback(data->font_changed_user_data);
}


static void on_font_size_changed(AdwSpinRow *spin, SettingsDialogData *data) {
    int val = (int)adw_spin_row_get_value(spin);
    if (val < 8)  val = 8;
    if (val > 48) val = 48;
    data->settings->font_size = val;
    settings_save(data->settings);
    if (data->font_changed_callback)
        data->font_changed_callback(data->font_changed_user_data);
}

static void on_render_style_row_changed(AdwComboRow *row, GParamSpec *pspec, SettingsDialogData *data) {
    (void)pspec;
    const char *styles[] = {"diction", "python", "goldendict-ng", "slate-card", "paper"};
    guint idx = adw_combo_row_get_selected(row);
    if (idx >= G_N_ELEMENTS(styles)) return;

    g_free(data->settings->render_style);
    data->settings->render_style = g_strdup(styles[idx]);
    settings_save(data->settings);
    if (data->font_changed_callback)
        data->font_changed_callback(data->font_changed_user_data);
}

/* ---- Dialog closed ---- */

static void on_dialog_closed(SettingsDialogData *data) {
    settings_save(data->settings);
    /* Do NOT call reload_callback here — it's only needed on explicit Rescan.
       Calling it unconditionally on close caused double-reload jank. */
    g_free(data->selected_dict_id);
    if (data->group_selection_ids)
        g_hash_table_unref(data->group_selection_ids);
    g_free(data);
}

/* ================================================================
   Public API
   ================================================================ */

GtkWidget* settings_dialog_new(GtkWindow *parent, AppSettings *settings,
                               AdwStyleManager *style_manager,
                               void (*reload_callback)(void *),
                               void (*soft_reload_callback)(void *),
                               void *user_data) {
    (void)parent;
    SettingsDialogData *data = g_new0(SettingsDialogData, 1);
    data->settings            = settings;
    data->group_selection_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    data->style_manager       = style_manager;
    data->parent_window       = parent;
    data->reload_callback     = reload_callback;
    data->soft_reload_callback = soft_reload_callback;
    data->user_data           = user_data;

    AdwDialog *dialog = adw_preferences_dialog_new();
    adw_preferences_dialog_set_search_enabled(ADW_PREFERENCES_DIALOG(dialog), FALSE);
    data->dialog = dialog;
    /* Store data pointer so settings_dialog_set_font_callback can find it */
    g_object_set_data(G_OBJECT(dialog), "sdd", data);

    /* ============================================================
       TAB 1 — Appearance
       ============================================================ */
    AdwPreferencesPage *appearance_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(appearance_page, "Appearance");
    adw_preferences_page_set_icon_name(appearance_page,
        "preferences-desktop-appearance-symbolic");
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), appearance_page);

    /* Theme group */
    AdwPreferencesGroup *theme_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(theme_group, "Theme");
    adw_preferences_group_set_description(theme_group, "Choose light, dark or follow the system");
    adw_preferences_page_add(appearance_page, theme_group);

    AdwComboRow *theme_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme_row), "Color Scheme");
    GtkStringList *theme_model = gtk_string_list_new(
        (const char *[]){"System Default", "Light", "Dark", NULL});
    adw_combo_row_set_model(theme_row, G_LIST_MODEL(theme_model));
    g_object_unref(theme_model);

    int theme_idx = 0;
    if (settings->theme && strcmp(settings->theme, "light") == 0) theme_idx = 1;
    else if (settings->theme && strcmp(settings->theme, "dark") == 0)  theme_idx = 2;
    adw_combo_row_set_selected(theme_row, theme_idx);
    
    g_signal_connect(theme_row, "notify::selected", G_CALLBACK(on_theme_row_changed), data);
    adw_preferences_group_add(theme_group, GTK_WIDGET(theme_row));

    /* Color Theme group */
    AdwPreferencesGroup *color_theme_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(color_theme_group, "Entry Colors");
    adw_preferences_group_set_description(color_theme_group, "Theme style for dictionary entries");
    adw_preferences_page_add(appearance_page, color_theme_group);

    AdwComboRow *color_theme_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(color_theme_row), "Color Preset");
    GtkStringList *color_model = gtk_string_list_new((const char *[]){
        "Default", "Solarized", "Dracula", "Nord", "Gruvbox", "Monokai", "Material", "Ocean", "Forest", "Sepia", NULL});
    adw_combo_row_set_model(color_theme_row, G_LIST_MODEL(color_model));
    g_object_unref(color_model);

    const char *themes[] = {"default", "solarized", "dracula", "nord", "gruvbox", "monokai", "material", "ocean", "forest", "sepia"};
    int color_idx = 0;
    for (int i=0; i<10; i++) {
        if (g_strcmp0(settings->color_theme, themes[i]) == 0) { color_idx = i; break; }
    }
    adw_combo_row_set_selected(color_theme_row, color_idx);
    g_signal_connect(color_theme_row, "notify::selected", G_CALLBACK(on_color_theme_row_changed), data);
    adw_preferences_group_add(color_theme_group, GTK_WIDGET(color_theme_row));

    AdwComboRow *render_style_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(render_style_row), "Entry Layout");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(render_style_row),
        "Switch between compact, card, and editorial dictionary layouts");
    GtkStringList *render_style_model = gtk_string_list_new((const char *[]){
        "Diction", "Python", "GoldenDict-ng", "Slate Card", "Paper", NULL});
    adw_combo_row_set_model(render_style_row, G_LIST_MODEL(render_style_model));
    g_object_unref(render_style_model);

    const char *render_styles[] = {"diction", "python", "goldendict-ng", "slate-card", "paper"};
    int render_style_idx = 0;
    for (int i = 0; i < 5; i++) {
        if (g_strcmp0(settings->render_style, render_styles[i]) == 0) {
            render_style_idx = i;
            break;
        }
    }
    adw_combo_row_set_selected(render_style_row, render_style_idx);
    g_signal_connect(render_style_row, "notify::selected", G_CALLBACK(on_render_style_row_changed), data);
    adw_preferences_group_add(color_theme_group, GTK_WIDGET(render_style_row));

    /* Font group */
    AdwPreferencesGroup *font_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(font_group, "Font");
    adw_preferences_group_set_description(font_group,
        "Font used to display dictionary definitions");
    adw_preferences_page_add(appearance_page, font_group);

    /* Font family dropdown — populated from all Pango/system font families */
    AdwComboRow *font_family_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(font_family_row), "Font Family");

    /* Enumerate system fonts via Pango */
    PangoFontMap *font_map = pango_cairo_font_map_get_default();
    PangoFontFamily **families = NULL;
    int n_families = 0;
    pango_font_map_list_families(font_map, &families, &n_families);

    /* Sort alphabetically (case-insensitive) */
    if (n_families > 1) {
        for (int i = 0; i < n_families - 1; i++) {
            for (int j = i + 1; j < n_families; j++) {
                const char *a = pango_font_family_get_name(families[i]);
                const char *b = pango_font_family_get_name(families[j]);
                if (g_ascii_strcasecmp(a, b) > 0) {
                    PangoFontFamily *tmp = families[i];
                    families[i] = families[j];
                    families[j] = tmp;
                }
            }
        }
    }

    /* Build string list and find current setting index */
    GtkStringList *font_model = gtk_string_list_new(NULL);
    int font_idx = 0;
    for (int i = 0; i < n_families; i++) {
        const char *fname = pango_font_family_get_name(families[i]);
        gtk_string_list_append(font_model, fname);
        if (g_strcmp0(settings->font_family, fname) == 0)
            font_idx = i;
    }
    g_free(families);

    adw_combo_row_set_model(font_family_row, G_LIST_MODEL(font_model));
    g_object_unref(font_model);
    adw_combo_row_set_selected(font_family_row, (guint)font_idx);
    g_signal_connect(font_family_row, "notify::selected", G_CALLBACK(on_font_family_row_changed), data);
    adw_preferences_group_add(font_group, GTK_WIDGET(font_family_row));

    /* Font size spin */
    GtkAdjustment *size_adj = gtk_adjustment_new(
        settings->font_size > 0 ? settings->font_size : 16,
        8, 48, 1, 4, 0);
    AdwSpinRow *font_size_row = ADW_SPIN_ROW(adw_spin_row_new(size_adj, 1, 0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(font_size_row), "Font Size");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(font_size_row), "Size in pixels (8 – 48)");
    g_signal_connect(font_size_row, "changed", G_CALLBACK(on_font_size_changed), data);
    adw_preferences_group_add(font_group, GTK_WIDGET(font_size_row));

    /* ============================================================
       TAB 2 — Dictionaries
       ============================================================ */
    AdwPreferencesPage *dict_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(dict_page, "Dictionaries");
    adw_preferences_page_set_icon_name(dict_page, "accessories-dictionary-symbolic");
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), dict_page);

    /* --- Directory group --- */
    AdwPreferencesGroup *dir_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(dir_group, "Dictionary Directories");
    adw_preferences_group_set_description(dir_group,
        "Diction scans these folders and auto-adds all supported dictionaries");
    adw_preferences_page_add(dict_page, dir_group);

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

    /* --- Dictionaries group --- */
    AdwPreferencesGroup *dict_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(dict_group, "Dictionaries");
    adw_preferences_group_set_description(dict_group,
        "Toggle enabled/disabled, reorder priority, or add individual files");
    adw_preferences_page_add(dict_page, dict_group);

    data->dict_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(data->dict_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(data->dict_list), "boxed-list");
    g_signal_connect(data->dict_list, "row-selected", G_CALLBACK(on_dict_row_selected), data);
    adw_preferences_group_add(dict_group, GTK_WIDGET(data->dict_list));

    GtkWidget *reorder_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    data->move_up_btn = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(data->move_up_btn, "Move selected dictionary up");
    DictMoveData *up_data = g_new(DictMoveData, 1);
    up_data->data      = data;
    up_data->direction = -1;
    up_data->id        = NULL;
    g_signal_connect_data(data->move_up_btn, "clicked", G_CALLBACK(on_move_dictionary),
        up_data, dict_move_data_destroy, 0);

    data->move_down_btn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(data->move_down_btn, "Move selected dictionary down");
    DictMoveData *down_data = g_new(DictMoveData, 1);
    down_data->data      = data;
    down_data->direction = 1;
    down_data->id        = NULL;
    g_signal_connect_data(data->move_down_btn, "clicked", G_CALLBACK(on_move_dictionary),
        down_data, dict_move_data_destroy, 0);

    data->create_group_btn = gtk_button_new_with_label("Create Group from Selected…");
    g_signal_connect(data->create_group_btn, "clicked",
        G_CALLBACK(on_create_group_from_selected), data);

    gtk_box_append(GTK_BOX(reorder_box), data->move_up_btn);
    gtk_box_append(GTK_BOX(reorder_box), data->move_down_btn);
    gtk_box_append(GTK_BOX(reorder_box), data->create_group_btn);
    adw_preferences_group_add(dict_group, reorder_box);

    GtkWidget *add_file_btn = gtk_button_new_with_label("Add Dictionary File…");
    gtk_widget_add_css_class(add_file_btn, "suggested-action");
    g_signal_connect(add_file_btn, "clicked", G_CALLBACK(on_add_dictionary_file), data);
    adw_preferences_group_add(dict_group, add_file_btn);

    GtkWidget *import_btn = gtk_button_new_with_label("Import Files…");
    gtk_widget_add_css_class(import_btn, "suggested-action");
    g_signal_connect(import_btn, "clicked", G_CALLBACK(on_import_dictionary_files), data);
    adw_preferences_group_add(dict_group, import_btn);

    /* --- Groups group --- */
    AdwPreferencesGroup *group_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group_group, "Dictionary Groups");
    adw_preferences_group_set_description(group_group,
        "Custom groups appear in the sidebar Groups tab");
    adw_preferences_page_add(dict_page, group_group);

    data->group_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(data->group_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(data->group_list), "boxed-list");
    adw_preferences_group_add(group_group, GTK_WIDGET(data->group_list));

    /* ---- Initial population ---- */
    update_dir_list(data);
    update_dict_list(data);
    update_group_list(data);
    refresh_move_buttons(data);

    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(on_dialog_closed), data);

    /* If debug auto-scan env var is set, start an integrated scan immediately
     * when the preferences dialog is created. This helps reproduce issues
     * without manual interaction. */
    if (getenv("DICTION_DEBUG_AUTO_SCAN")) {
        if (data->settings->dictionary_dirs->len > 0) {
            int n = (int)data->settings->dictionary_dirs->len;
            char **dirs = g_new0(char*, n + 1);
            for (int i = 0; i < n; i++) dirs[i] = g_strdup(g_ptr_array_index(data->settings->dictionary_dirs, i));
            dirs[n] = NULL;
            show_scan_dialog_for_dirs(data, dirs, n, TRUE);
        }
    }

    return GTK_WIDGET(dialog);
}

/* Allow callers to register a font-change callback (called immediately when
   font family or size changes so the webkit view can update live). */
void settings_dialog_set_font_callback(GtkWidget *dialog_widget,
                                       void (*cb)(void *), void *user_data) {
    /* The dialog is an AdwPreferencesDialog; we stashed SettingsDialogData via
       the "closed" signal's swapped data pointer — recover it via g_object_get_data. */
    SettingsDialogData *data = g_object_get_data(G_OBJECT(dialog_widget), "sdd");
    if (data) {
        data->font_changed_callback   = cb;
        data->font_changed_user_data  = user_data;
    }
}

void settings_dialog_run(GtkWidget *dialog) {
    (void)dialog;
}
