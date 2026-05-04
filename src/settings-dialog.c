#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <webkit/webkit.h>
#include <pango/pangocairo.h>
#include <adwaita.h>
#include "json-theme.h"

/* Normalize path strings used as keys in the scanning UI. This ensures the
 * same canonical form is used when creating rows and when progress updates
 * are reported from extraction code. Returns a newly-allocated string. */
/* Normalize path strings used as keys in the scanning UI.
 * This version avoids expensive disk I/O when possible. */
static char *normalize_scan_path(const char *path) {
    if (!path) return g_strdup("");
    /* If it's already an absolute path and doesn't contain "..",
     * we skip full canonicalization to avoid disk I/O in tight loops. */
    if (path[0] == '/' && !strstr(path, "/./") && !strstr(path, "/../")) {
        return g_utf8_make_valid(path, -1);
    }
    char *valid = g_utf8_make_valid(path, -1);
    char *canon = g_canonicalize_filename(valid, NULL);
    g_free(valid);
    if (!canon) return g_strdup("");
    return canon;
}

static GtkWidget *new_plain_action_row(void) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    return row;
}

typedef struct _SettingsDialogData {
    AdwDialog          *dialog;
    AppSettings        *settings;
    GtkListBox         *dir_list;
    GtkListBox         *dict_list;
    GtkListBox         *group_list;
    AdwActionRow       *dict_library_row;
    AdwActionRow       *dict_selection_row;
    AdwActionRow       *dict_activity_row;
    AdwBanner          *dict_banner;
    GtkWidget          *move_up_btn;
    GtkWidget          *move_down_btn;
    GtkWidget          *create_group_btn;
    char               *selected_dict_id;
    gboolean            rebuilding_dict_list;
    GHashTable         *group_selection_ids;
    guint               banner_timeout_id;
    guint               session_added_count;
    guint               session_imported_count;
    guint               session_removed_count;
    gboolean            closed;
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
static void refresh_dictionary_lists(SettingsDialogData *data);
static void update_dictionary_overview(SettingsDialogData *data);
static void present_dictionary_feedback(SettingsDialogData *data,
                                        guint added_delta,
                                        guint imported_delta,
                                        guint removed_delta,
                                        const char *title);
static gboolean settings_dialog_is_active(SettingsDialogData *data);
static char *format_name_list(GPtrArray *names);
static void settings_dialog_set_selected_dict_id(SettingsDialogData *data, const char *id);

static gboolean settings_dialog_is_active(SettingsDialogData *data) {
    return data && !data->closed;
}

static void settings_dialog_set_selected_dict_id(SettingsDialogData *data, const char *id) {
    if (!data) {
        return;
    }

    g_clear_pointer(&data->selected_dict_id, g_free);
    if (id && *id) {
        data->selected_dict_id = g_strdup(id);
    }
}

static void refresh_dictionary_lists(SettingsDialogData *data) {
    if (!settings_dialog_is_active(data)) {
        return;
    }

    update_dir_list(data);
    update_dict_list(data);
    update_group_list(data);
    refresh_move_buttons(data);
}

static void settings_dialog_data_free(gpointer user_data) {
    SettingsDialogData *data = user_data;
    if (!data) {
        return;
    }

    if (data->banner_timeout_id != 0) {
        g_source_remove(data->banner_timeout_id);
    }

    g_clear_pointer(&data->selected_dict_id, g_free);
    if (data->group_selection_ids) {
        g_hash_table_unref(data->group_selection_ids);
    }
    g_free(data);
}

static char *format_name_list(GPtrArray *names) {
    if (!names || names->len == 0) {
        return g_strdup("");
    }

    GString *buf = g_string_new("");
    guint shown = MIN(names->len, 3);
    for (guint i = 0; i < shown; i++) {
        const char *name = g_ptr_array_index(names, i);
        if (i > 0) {
            g_string_append(buf, ", ");
        }
        g_string_append(buf, name ? name : "(unknown)");
    }

    if (names->len > shown) {
        g_string_append_printf(buf, " +%u more", names->len - shown);
    }

    return g_string_free(buf, FALSE);
}

static gboolean dialog_path_is_inside_dir(const char *path, const char *dir_path) {
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

static gboolean hide_dictionary_feedback_banner(gpointer user_data) {
    SettingsDialogData *data = user_data;
    if (!settings_dialog_is_active(data)) {
        return G_SOURCE_REMOVE;
    }
    data->banner_timeout_id = 0;
    if (data->dict_banner) {
        adw_banner_set_revealed(data->dict_banner, FALSE);
    }
    return G_SOURCE_REMOVE;
}

static void update_dictionary_overview(SettingsDialogData *data) {
    if (!settings_dialog_is_active(data)) {
        return;
    }

    guint total = data->settings ? data->settings->dictionaries->len : 0;
    guint enabled = 0;
    for (guint i = 0; data->settings && i < data->settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(data->settings->dictionaries, i);
        if (cfg->enabled) {
            enabled++;
        }
    }

    if (data->dict_library_row) {
        char *subtitle = g_strdup_printf("%u total • %u enabled • %u disabled • %u folders watched",
                                         total,
                                         enabled,
                                         total >= enabled ? total - enabled : 0,
                                         data->settings ? data->settings->dictionary_dirs->len : 0);
        adw_action_row_set_subtitle(data->dict_library_row, subtitle);
        g_free(subtitle);
    }

    if (data->dict_selection_row) {
        guint grouped = data->group_selection_ids ? g_hash_table_size(data->group_selection_ids) : 0;
        char *subtitle = NULL;
        if (data->selected_dict_id || grouped > 0) {
            if (grouped > 0) {
                subtitle = g_strdup_printf("%s • %u checked for group creation",
                                           data->selected_dict_id ? "1 row selected for priority"
                                                                  : "No row selected for priority",
                                           grouped);
            } else {
                subtitle = g_strdup(data->selected_dict_id
                    ? "1 row selected for priority."
                    : "No row selected for priority.");
            }
        } else {
            subtitle = g_strdup("Click a row to reorder priority. Tick checkboxes to build a custom group.");
        }
        adw_action_row_set_subtitle(data->dict_selection_row, subtitle);
        g_free(subtitle);
    }

    if (data->dict_activity_row) {
        char *subtitle = NULL;
        if (data->session_added_count == 0 &&
            data->session_imported_count == 0 &&
            data->session_removed_count == 0) {
            subtitle = g_strdup("No dictionary changes yet in this session.");
        } else {
            subtitle = g_strdup_printf("This session: %u added • %u imported • %u removed",
                                       data->session_added_count,
                                       data->session_imported_count,
                                       data->session_removed_count);
        }
        adw_action_row_set_subtitle(data->dict_activity_row, subtitle);
        g_free(subtitle);
    }
}

static void present_dictionary_feedback(SettingsDialogData *data,
                                        guint added_delta,
                                        guint imported_delta,
                                        guint removed_delta,
                                        const char *title) {
    if (!settings_dialog_is_active(data)) {
        return;
    }

    data->session_added_count += added_delta;
    data->session_imported_count += imported_delta;
    data->session_removed_count += removed_delta;
    update_dictionary_overview(data);

    if (!data->dict_banner || !title || !*title) {
        return;
    }

    adw_banner_set_title(data->dict_banner, title);
    adw_banner_set_revealed(data->dict_banner, TRUE);

    if (data->banner_timeout_id != 0) {
        g_source_remove(data->banner_timeout_id);
    }
    data->banner_timeout_id = g_timeout_add_seconds(6, hide_dictionary_feedback_banner, data);
}


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
    if (!settings_dialog_is_active(data)) {
        g_object_unref(chooser);
        return;
    }
    GError *error = NULL;
    GFile *file = gtk_file_dialog_select_folder_finish(chooser, result, &error);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            /* Add to settings so the main loader picks it up. */
            guint before = data->settings->dictionary_dirs->len;
            settings_add_directory(data->settings, path);

            /* Show the scan dialog IMMEDIATELY before doing heavy UI work in the main window */
            char **dirs = g_new0(char *, 2);
            dirs[0] = g_strdup(path);
            dirs[1] = NULL;
            extern void show_scan_dialog_for_dirs(SettingsDialogData *data, char **dirs, int n_dirs, gboolean call_reload);
            show_scan_dialog_for_dirs(data, dirs, 1, TRUE);

            /* Update the dir list in preferences */
            update_dir_list(data);

            char *name = g_path_get_basename(path);
            char *message = data->settings->dictionary_dirs->len > before
                ? g_strdup_printf("Added folder: %s", name)
                : g_strdup_printf("Folder already added: %s", name);
            present_dictionary_feedback(data, 0, 0, 0, message);
            g_free(message);
            g_free(name);

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
    if (!settings_dialog_is_active(data)) {
        return G_SOURCE_REMOVE;
    }
    update_dir_list(data);
    update_dict_list(data);
    update_group_list(data);
    return G_SOURCE_REMOVE;
}

static void on_remove_directory_clicked(GtkButton *btn, DirRemoveData *d) {
    (void)btn;
    if (!settings_dialog_is_active(d->data)) {
        return;
    }
    guint before = d->data->settings->dictionaries->len;
    GPtrArray *removed_names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < d->data->settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(d->data->settings->dictionaries, i);
        if (dialog_path_is_inside_dir(cfg->path, d->path)) {
            g_ptr_array_add(removed_names, g_strdup(cfg->name));
        }
    }
    settings_remove_directory(d->data->settings, d->path);
    guint removed = before > d->data->settings->dictionaries->len
        ? before - d->data->settings->dictionaries->len
        : 0;
    char *dir_name = g_path_get_basename(d->path);
    char *removed_list = format_name_list(removed_names);
    char *message = NULL;
    if (removed > 0 && removed_list[0] != '\0') {
        message = g_strdup_printf("Removed folder %s and %u dictionaries: %s",
                                  dir_name, removed, removed_list);
    } else {
        message = g_strdup_printf("Removed folder: %s", dir_name);
    }
    present_dictionary_feedback(d->data, 0, 0, removed, message);
    g_free(message);
    g_free(removed_list);
    g_free(dir_name);
    g_ptr_array_free(removed_names, TRUE);
    if (d->data->reload_callback) d->data->reload_callback(d->data->user_data);
    g_idle_add(on_remove_directory_idle, d->data);
}

static void on_rescan_directories(GtkButton *btn, SettingsDialogData *data) {
    (void)btn;
    if (!settings_dialog_is_active(data)) {
        return;
    }
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
    for (int i = 0; i < n; i++) {
        const char *dir_path = g_ptr_array_index(data->settings->dictionary_dirs, i);
        dirs[i] = g_strdup(dir_path);
        
        /* Clear ignored paths in this directory so they can be rediscovered */
        for (gint j = (gint)data->settings->ignored_dictionary_paths->len - 1; j >= 0; j--) {
            const char *ignored_path = g_ptr_array_index(data->settings->ignored_dictionary_paths, (guint)j);
            if (path_is_inside_dir(ignored_path, dir_path)) {
                g_ptr_array_remove_index(data->settings->ignored_dictionary_paths, (guint)j);
            }
        }
    }
    dirs[n] = NULL;
    extern void force_next_dictionary_directory_rescan(void);
    force_next_dictionary_directory_rescan();
    extern void show_scan_dialog_for_dirs(SettingsDialogData *data, char **dirs, int n_dirs, gboolean call_reload);
    show_scan_dialog_for_dirs(data, dirs, n, TRUE);
}

/* ================================================================
   FTS Index Builder dialog
   ================================================================ */

#include "dict-fts-index.h"
#include "dict-mmap.h"
#include "flat-index.h"
#include "dict-loader.h"

/* Rows between COMMIT cycles during indexing */
#define FTS_BATCH_SIZE 500

/* Declared in main.c — snapshot of currently loaded dicts for FTS building */
extern GPtrArray* collect_fts_build_entries(void); /* GPtrArray<DictEntry*>, each ref'd */

typedef struct {
    SettingsDialogData *sdd;
    AdwDialog          *dialog;
    GtkListBox         *list;
    GtkLabel           *status_label;
    GtkWidget          *start_btn;
    GtkWidget          *stop_btn;
    GtkWidget          *close_btn;
    volatile gint       cancel;    /* 0=run, 1=stop */
    GThread            *worker;
    GPtrArray          *to_index;  /* DictEntry* list (reffed by us) */
    gint                closed;
} FtsIndexCtx;

typedef struct {
    FtsIndexCtx *ctx;
    char        *status;     /* newly allocated, main thread frees */
    char        *path;       /* newly allocated */
    gboolean     done;
    gboolean     dict_done;
    guint        entry_done;
    guint        entry_total;
} FtsIdleData;

static gboolean fts_idle_update(gpointer user_data)
{
    FtsIdleData *d = user_data;
    FtsIndexCtx *ctx = d->ctx;

    if (!g_atomic_int_get(&ctx->closed)) {
        if (ctx->status_label && d->status)
            gtk_label_set_text(ctx->status_label, d->status);

        /* Update per-row progress label */
        if (d->path && ctx->list) {
            GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_index(ctx->list, 0));
            /* Walk rows to find matching path */
            for (int i = 0; ; i++) {
                GtkListBoxRow *lr = gtk_list_box_get_row_at_index(ctx->list, i);
                if (!lr) break;
                const char *rpath = g_object_get_data(G_OBJECT(lr), "dict-path");
                if (g_strcmp0(rpath, d->path) == 0) {
                    row = GTK_WIDGET(lr);
                    break;
                }
            }
            if (row) {
                GtkWidget *prog = g_object_get_data(G_OBJECT(row), "prog-label");
                GtkWidget *spin = g_object_get_data(G_OBJECT(row), "row-spin");
                if (d->dict_done) {
                    if (prog) gtk_label_set_text(GTK_LABEL(prog), "✓ Indexed");
                    if (spin) {
                        gtk_spinner_stop(GTK_SPINNER(spin));
                        gtk_widget_set_visible(spin, FALSE);
                    }
                } else if (d->entry_total > 0) {
                    char *txt = g_strdup_printf("%u / %u", d->entry_done, d->entry_total);
                    if (prog) gtk_label_set_text(GTK_LABEL(prog), txt);
                    g_free(txt);
                    if (spin) {
                        gtk_spinner_start(GTK_SPINNER(spin));
                        gtk_widget_set_visible(spin, TRUE);
                    }
                }
            }
        }

        if (d->done) {
            if (ctx->stop_btn)  gtk_widget_set_sensitive(ctx->stop_btn, FALSE);
            if (ctx->start_btn) gtk_widget_set_sensitive(ctx->start_btn, TRUE);
            if (ctx->close_btn) {
                gtk_widget_set_sensitive(ctx->close_btn, TRUE);
                gtk_widget_add_css_class(ctx->close_btn, "suggested-action");
            }
        }
    }

    g_free(d->status);
    g_free(d->path);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static void fts_post(FtsIndexCtx *ctx, const char *status, const char *path,
                     gboolean done, gboolean dict_done,
                     guint edone, guint etotal)
{
    FtsIdleData *d = g_new0(FtsIdleData, 1);
    d->ctx        = ctx;
    d->status     = status ? g_strdup(status) : NULL;
    d->path       = path   ? g_strdup(path)   : NULL;
    d->done       = done;
    d->dict_done  = dict_done;
    d->entry_done = edone;
    d->entry_total= etotal;
    g_idle_add(fts_idle_update, d);
}

static gpointer fts_build_worker(gpointer user_data)
{
    FtsIndexCtx *ctx = user_data;
    GPtrArray   *list = ctx->to_index;
    guint        total = list ? list->len : 0;

    for (guint di = 0; di < total; di++) {
        if (g_atomic_int_get(&ctx->cancel)) break;

        /* DictEntry* — we only need name, path, and dict (DictMmap) */
        DictEntry *e = g_ptr_array_index(list, di);

        const char *dpath = e->path;
        const char *dname = e->name;

        char *st = g_strdup_printf("Indexing %u of %u: %s", di + 1, total, dname ? dname : "?");
        fts_post(ctx, st, dpath, FALSE, FALSE, 0, 0);
        g_free(st);

        /* Open builder */
        GError *err = NULL;
        DictFtsBuilder *builder = dict_fts_builder_new(dpath, &err);
        if (!builder) {
            fprintf(stderr, "[FTS Builder] %s\n", err ? err->message : "?");
            g_clear_error(&err);
            fts_post(ctx, NULL, dpath, FALSE, TRUE, 0, 0);
            continue;
        }

        /* Access flat index via the DictMmap stored in DictEntry */
        struct DictMmap *dmmap = e->dict;

        if (!dmmap || !dmmap->data || !dmmap->index) {
            dict_fts_builder_abort(builder);
            fts_post(ctx, NULL, dpath, FALSE, TRUE, 0, 0);
            continue;
        }

        size_t count = flat_index_count(dmmap->index);
        guint batch = 0;

        for (size_t i = 0; i < count; i++) {
            if (g_atomic_int_get(&ctx->cancel)) break;

            const FlatTreeEntry *ent = flat_index_get(dmmap->index, i);
            if (!ent) continue;

            const char *hw  = dmmap->data + ent->h_off;
            gsize        hwl = ent->h_len;
            const char *def = NULL;
            gsize        dl  = 0;
            char        *to_free = NULL;

            if (ent->d_len > 0) {
                extern const char* dict_get_definition(struct DictMmap*, const FlatTreeEntry*, size_t*, char**);
                def = dict_get_definition(dmmap, ent, &dl, &to_free);
            }

            dict_fts_builder_add(builder, (guint)i, hw, hwl, def, dl);
            g_free(to_free);

            batch++;
            if (batch >= FTS_BATCH_SIZE) {
                dict_fts_builder_commit_batch(builder);
                batch = 0;
                if ((i & 0x3FF) == 0)
                    fts_post(ctx, NULL, dpath, FALSE, FALSE, (guint)i, (guint)count);
            }
        }

        if (g_atomic_int_get(&ctx->cancel)) {
            dict_fts_builder_abort(builder);
        } else {
            gboolean ok = dict_fts_builder_finish(builder, &err);
            if (!ok) {
                fprintf(stderr, "[FTS Builder] finish: %s\n", err ? err->message : "?");
                g_clear_error(&err);
            }
        }

        fts_post(ctx, NULL, dpath, FALSE, TRUE, 0, 0);
    }

    char *final_st = g_atomic_int_get(&ctx->cancel)
        ? g_strdup("Indexing stopped.")
        : g_strdup_printf("Done — %u dictionar%s indexed.",
                          total, total == 1 ? "y" : "ies");
    fts_post(ctx, final_st, NULL, TRUE, FALSE, 0, 0);
    g_free(final_st);

    /* Unref all entries */
    if (list) {
        for (guint i = 0; i < list->len; i++) {
            dict_entry_unref(g_ptr_array_index(list, i));
        }
        g_ptr_array_free(list, TRUE);
        ctx->to_index = NULL;
    }

    return NULL;
}

static void on_fts_start_clicked(GtkButton *btn, FtsIndexCtx *ctx)
{
    (void)btn;
    if (ctx->worker) return;

    /* Collect checked rows */
    GPtrArray *selected = g_ptr_array_new();
    for (int i = 0; ; i++) {
        GtkListBoxRow *lr = gtk_list_box_get_row_at_index(ctx->list, i);
        if (!lr) break;
        GtkWidget *cb = g_object_get_data(G_OBJECT(lr), "row-check");
        if (cb && gtk_check_button_get_active(GTK_CHECK_BUTTON(cb))) {
            const char *path = g_object_get_data(G_OBJECT(lr), "dict-path");
            if (path) g_ptr_array_add(selected, (char *)path); /* borrowed */
        }
    }

    /* Build to_index from loaded dicts matching selected paths */
    GPtrArray *snapshot = collect_fts_build_entries(); /* each entry ref'd */
    GPtrArray *to_index = g_ptr_array_new();
    if (snapshot) {
        for (guint i = 0; i < snapshot->len; i++) {
            DictEntry *e = g_ptr_array_index(snapshot, i);
            for (guint j = 0; j < selected->len; j++) {
                if (g_strcmp0(e->path, g_ptr_array_index(selected, j)) == 0) {
                    g_ptr_array_add(to_index, e); /* transfer ref */
                    break;
                }
            }
        }
        /* Unref entries NOT added to to_index */
        for (guint i = 0; i < snapshot->len; i++) {
            void *e = g_ptr_array_index(snapshot, i);
            gboolean found = FALSE;
            for (guint j = 0; j < to_index->len; j++) {
                if (g_ptr_array_index(to_index, j) == e) { found = TRUE; break; }
            }
            if (!found) {
                dict_entry_unref(e);
            }
        }
        g_ptr_array_free(snapshot, TRUE);
    }
    g_ptr_array_free(selected, TRUE);

    if (to_index->len == 0) {
        g_ptr_array_free(to_index, TRUE);
        if (ctx->status_label)
            gtk_label_set_text(ctx->status_label, "No dictionaries selected.");
        return;
    }

    ctx->to_index = to_index;
    g_atomic_int_set(&ctx->cancel, 0);
    gtk_widget_set_sensitive(ctx->start_btn, FALSE);
    gtk_widget_set_sensitive(ctx->stop_btn, TRUE);
    if (ctx->close_btn) gtk_widget_set_sensitive(ctx->close_btn, FALSE);
    if (ctx->status_label) gtk_label_set_text(ctx->status_label, "Starting…");
    ctx->worker = g_thread_new("fts-builder", fts_build_worker, ctx);
}

static void on_fts_stop_clicked(GtkButton *btn, FtsIndexCtx *ctx)
{
    (void)btn;
    g_atomic_int_set(&ctx->cancel, 1);
    gtk_widget_set_sensitive(ctx->stop_btn, FALSE);
}

static void on_fts_dialog_closed(FtsIndexCtx *ctx)
{
    g_atomic_int_set(&ctx->cancel, 1);
    g_atomic_int_set(&ctx->closed, 1);
    if (ctx->worker) {
        g_thread_join(ctx->worker);
        ctx->worker = NULL;
    }
    if (ctx->to_index) {
        for (guint i = 0; i < ctx->to_index->len; i++) {
            dict_entry_unref(g_ptr_array_index(ctx->to_index, i));
        }
        g_ptr_array_free(ctx->to_index, TRUE);
        ctx->to_index = NULL;
    }
    g_free(ctx);
}

static void show_fts_builder_dialog(SettingsDialogData *sdd, GtkWindow *parent)
{
    FtsIndexCtx *ctx = g_new0(FtsIndexCtx, 1);
    ctx->sdd    = sdd;
    ctx->cancel = 0;
    ctx->closed = 0;

    AdwDialog *dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, "Build Full Text Search Index");
    adw_dialog_set_content_width(dialog, 520);
    adw_dialog_set_content_height(dialog, 460);
    ctx->dialog = dialog;

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_dialog_set_child(dialog, outer);

    /* Header toolbar */
    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(hb));

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(body, 18);
    gtk_widget_set_margin_end(body, 18);
    gtk_widget_set_margin_top(body, 12);
    gtk_widget_set_margin_bottom(body, 18);
    gtk_box_append(GTK_BOX(outer), body);

    GtkWidget *desc = gtk_label_new(
        "Select dictionaries to build a persistent SQLite FTS index.\n"
        "Already-indexed dictionaries are pre-deselected.");
    gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(desc), 0.0f);
    gtk_widget_add_css_class(desc, "dim-label");
    gtk_box_append(GTK_BOX(body), desc);

    /* Dict list */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(body), scroll);

    GtkListBox *list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(list), "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(list));
    ctx->list = list;

    /* Populate rows from settings dict list */
    if (sdd && sdd->settings) {
        for (guint i = 0; i < sdd->settings->dictionaries->len; i++) {
            DictConfig *cfg = g_ptr_array_index(sdd->settings->dictionaries, i);
            if (!cfg->enabled) continue;

            gboolean already = dict_fts_index_exists(cfg->path);

            GtkWidget *row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          cfg->name && *cfg->name ? cfg->name : cfg->path);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), cfg->path);
            g_object_set_data_full(G_OBJECT(row), "dict-path", g_strdup(cfg->path), g_free);

            /* Spinner (hidden initially) */
            GtkWidget *spin = gtk_spinner_new();
            gtk_widget_set_visible(spin, FALSE);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), spin);
            g_object_set_data(G_OBJECT(row), "row-spin", spin);

            /* Progress/status label */
            GtkWidget *prog = gtk_label_new(already ? "✓ Indexed" : "Not indexed");
            gtk_widget_add_css_class(prog, "dim-label");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), prog);
            g_object_set_data(G_OBJECT(row), "prog-label", prog);

            /* Checkbox — pre-checked only if not already indexed */
            GtkWidget *cb = gtk_check_button_new();
            gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), !already);
            adw_action_row_add_prefix(ADW_ACTION_ROW(row), cb);
            adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), cb);
            g_object_set_data(G_OBJECT(row), "row-check", cb);

            gtk_list_box_append(list, row);
        }
    }

    /* Status label */
    ctx->status_label = GTK_LABEL(gtk_label_new("Select dictionaries and press Start Indexing."));
    gtk_label_set_xalign(ctx->status_label, 0.0f);
    gtk_label_set_wrap(GTK_LABEL(ctx->status_label), TRUE);
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(ctx->status_label));

    /* Button row */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(body), btn_box);

    ctx->close_btn = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(ctx->close_btn, "clicked",
                             G_CALLBACK(adw_dialog_force_close), dialog);
    gtk_box_append(GTK_BOX(btn_box), ctx->close_btn);

    ctx->stop_btn = gtk_button_new_with_label("Stop");
    gtk_widget_add_css_class(ctx->stop_btn, "destructive-action");
    gtk_widget_set_sensitive(ctx->stop_btn, FALSE);
    g_signal_connect(ctx->stop_btn, "clicked", G_CALLBACK(on_fts_stop_clicked), ctx);
    gtk_box_append(GTK_BOX(btn_box), ctx->stop_btn);

    ctx->start_btn = gtk_button_new_with_label("Start Indexing");
    gtk_widget_add_css_class(ctx->start_btn, "suggested-action");
    g_signal_connect(ctx->start_btn, "clicked", G_CALLBACK(on_fts_start_clicked), ctx);
    gtk_box_append(GTK_BOX(btn_box), ctx->start_btn);

    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(on_fts_dialog_closed), ctx);
    adw_dialog_present(dialog, parent ? GTK_WIDGET(parent) : NULL);
}

static void on_fts_switch_toggled(GtkSwitch *sw, GParamSpec *pspec,
                                  SettingsDialogData *data)
{
    (void)pspec;
    gboolean active = gtk_switch_get_active(sw);
    data->settings->fts_enabled = active;
    settings_save(data->settings);

    if (active) {
        show_fts_builder_dialog(data, data->parent_window);
    }
}

static void on_fts_manage_clicked(AdwButtonRow *row, SettingsDialogData *data)
{
    (void)row;
    show_fts_builder_dialog(data, data->parent_window);
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
    int processed_count;
    int failed_count;
    int total_dirs;
    gboolean call_reload;
    gboolean cancelled;
    gboolean scan_done_received;
    gboolean completion_handled;
    /* When integrating with main loader, only show entries from these dirs */
    char **scan_dirs;
    int n_scan_dirs;
    GHashTable *row_map; /* map path -> GtkWidget* row for progress updates */
    gint ref_count;
    gint closed;
} ScanContext;

typedef enum {
    SCAN_EVENT_DISCOVERED = 0,
    SCAN_EVENT_LOAD_START,
    SCAN_EVENT_LOAD_SUCCESS,
    SCAN_EVENT_LOAD_FAILURE,
    SCAN_EVENT_IMPORT_START,
    SCAN_EVENT_IMPORT_SUCCESS,
    SCAN_EVENT_IMPORT_FAILURE
} ScanIdleEvent;

typedef struct {
    ScanContext *ctx;
    char *name;
    char *path;
    int event_type; /* ScanIdleEvent or DictLoaderEventType */
    gboolean is_progress_update;
    gboolean is_status_update;
    int progress;
} ScanIdleData;

typedef struct {
    ScanContext *ctx;
    char **dirs;
    int n_dirs;
    GListModel *files; /* Opt: if set, we handle these files */
    gboolean is_import;
} ScanThreadArgs;

/* Active scan contexts registered when integrating with the main loader */
static GList *active_scan_contexts = NULL;
static GMutex active_scan_contexts_mutex;

static void on_scan_cancel_clicked(GtkButton *btn, ScanContext *ctx);
static void on_scan_close_clicked(GtkButton *btn, ScanContext *ctx);
static void on_scan_dialog_closed(ScanContext *ctx);

static ScanContext *scan_context_ref(ScanContext *ctx) {
    if (ctx) {
        g_atomic_int_inc(&ctx->ref_count);
    }
    return ctx;
}

static void scan_context_unref(ScanContext *ctx) {
    if (!ctx || !g_atomic_int_dec_and_test(&ctx->ref_count)) {
        return;
    }

    if (ctx->scan_dirs) {
        for (int i = 0; i < ctx->n_scan_dirs; i++) {
            g_free(ctx->scan_dirs[i]);
        }
        g_free(ctx->scan_dirs);
    }
    if (ctx->row_map) {
        g_hash_table_destroy(ctx->row_map);
    }
    g_free(ctx);
}

static gboolean scan_context_is_closed(ScanContext *ctx) {
    return !ctx || g_atomic_int_get(&ctx->closed) != 0;
}

static gboolean scan_context_matches_path(ScanContext *ctx,
                                          const char *canonical_path,
                                          int event_type) {
    if (!ctx || !ctx->call_reload) {
        return FALSE;
    }
    if (event_type == -1) {
        return TRUE;
    }
    if (!canonical_path || !*canonical_path) {
        return FALSE;
    }
    if (!ctx->scan_dirs || ctx->n_scan_dirs == 0) {
        return TRUE;
    }

    for (int i = 0; i < ctx->n_scan_dirs; i++) {
        if (ctx->scan_dirs[i] && g_str_has_prefix(canonical_path, ctx->scan_dirs[i])) {
            return TRUE;
        }
    }

    return FALSE;
}

static void scan_context_update_processing_status(ScanContext *ctx) {
    if (scan_context_is_closed(ctx) || !ctx->status_label) {
        return;
    }

    char buf[256];
    if (ctx->processed_count < ctx->found_count) {
        g_snprintf(buf, sizeof(buf), "Loading %d of %d…",
                   ctx->processed_count + 1, ctx->found_count);
    } else if (ctx->found_count > 0) {
        g_snprintf(buf, sizeof(buf), "Found %d dictionaries", ctx->found_count);
    } else {
        g_snprintf(buf, sizeof(buf), "Scanning for dictionaries…");
    }
    gtk_label_set_text(GTK_LABEL(ctx->status_label), buf);
}

static void scan_context_finish_ui_if_ready(ScanContext *ctx) {
    if (scan_context_is_closed(ctx) || ctx->completion_handled ||
        !ctx->scan_done_received || ctx->processed_count < ctx->found_count) {
        return;
    }

    ctx->completion_handled = TRUE;

    gtk_spinner_stop(GTK_SPINNER(ctx->spinner));

    char buf[256];
    if (ctx->failed_count > 0) {
        g_snprintf(buf, sizeof(buf), "%s — %d loaded, %d failed",
                   ctx->cancelled ? "Scan canceled" : "Scan complete",
                   ctx->found_count - ctx->failed_count,
                   ctx->failed_count);
    } else {
        g_snprintf(buf, sizeof(buf), "%s — %d dictionaries found",
                   ctx->cancelled ? "Scan canceled" : "Scan complete",
                   ctx->found_count);
    }
    gtk_label_set_text(GTK_LABEL(ctx->status_label), buf);

    if (ctx->close_btn) {
        gtk_widget_set_sensitive(ctx->close_btn, TRUE);
        gtk_widget_add_css_class(ctx->close_btn, "suggested-action");
    }
    if (ctx->cancel_btn) {
        gtk_widget_set_sensitive(ctx->cancel_btn, FALSE);
    }

    if (!ctx->cancelled && ctx->call_reload &&
        ctx->data && settings_dialog_is_active(ctx->data)) {
        refresh_dictionary_lists(ctx->data);
    }
    if (!ctx->cancelled && !ctx->call_reload &&
        ctx->data && settings_dialog_is_active(ctx->data) && ctx->data->reload_callback) {
        ctx->data->reload_callback(ctx->data->user_data);
    }
}

static gboolean scan_idle_add_entry(gpointer user_data) {
    ScanIdleData *sid = user_data;
    ScanContext *ctx = sid->ctx;

    if (scan_context_is_closed(ctx)) {
        g_free(sid->name);
        g_free(sid->path);
        scan_context_unref(ctx);
        g_free(sid);
        return G_SOURCE_REMOVE;
    }

    if (sid->is_status_update) {
        if (ctx->status_label) {
            gtk_label_set_text(GTK_LABEL(ctx->status_label), sid->name ? sid->name : "");
        }
    } else if (sid->is_progress_update) {
        /* Progress update for an existing row */
        if (sid->path && ctx->row_map) {
            gpointer val = g_hash_table_lookup(ctx->row_map, sid->path);
            if (val) {
                GtkWidget *row = val;
                char buf[128];
                g_snprintf(buf, sizeof(buf), "%d%%", sid->progress);
                
                GtkWidget *progress_label = g_object_get_data(G_OBJECT(row), "scan-progress-label");
                if (progress_label) {
                    gtk_label_set_text(GTK_LABEL(progress_label), buf);
                    gtk_widget_set_visible(progress_label, TRUE);
                }

                if (sid->progress < 100) {
                    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Extracting...");
                    /* Move active row to top if it's currently processing */
                    g_object_ref(row);
                    gtk_list_box_remove(ctx->list, row);
                    gtk_list_box_prepend(ctx->list, row);
                    g_object_unref(row);
                }
            }
        }
    } else if (sid->event_type == SCAN_EVENT_DISCOVERED || sid->event_type == DICT_LOADER_EVENT_DISCOVERED) {
        /* Add a new dictionary row as soon as it's found (scan) or added (file). */
        if (sid->path && ctx->row_map) {
            gpointer existing = g_hash_table_lookup(ctx->row_map, sid->path);
            if (existing) {
                g_free(sid->name);
                g_free(sid->path);
                scan_context_unref(ctx);
                g_free(sid);
                return G_SOURCE_REMOVE;
            }
        }

        GtkWidget *row = new_plain_action_row();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sid->name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Queued");

        /* Per-row spinner — starts stopped, becomes visible when loading begins */
        GtkWidget *row_spinner = gtk_spinner_new();
        gtk_widget_set_visible(row_spinner, FALSE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), row_spinner);
        g_object_set_data(G_OBJECT(row), "scan-row-spinner", row_spinner);

        /* Legacy progress label kept for compat with progress-update path */
        GtkWidget *progress_label = gtk_label_new("");
        gtk_widget_add_css_class(progress_label, "dim-label");
        gtk_widget_set_visible(progress_label, FALSE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), progress_label);
        g_object_set_data(G_OBJECT(row), "scan-progress-label", progress_label);

        gtk_list_box_append(ctx->list, GTK_WIDGET(row));

        if (sid->path) {
            if (!ctx->row_map) ctx->row_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            g_hash_table_replace(ctx->row_map, g_strdup(sid->path), row);
        }
        ctx->found_count++;
        scan_context_update_processing_status(ctx);
        
        /* If it's an import-start event, immediately trigger the started-import UI */
        if (sid->event_type == SCAN_EVENT_IMPORT_START) sid->event_type = SCAN_EVENT_IMPORT_START; 
        /* fall through logic conceptually... but here we just continue to next blocks if we didn't exit */
    }
    
    if (sid->event_type == SCAN_EVENT_LOAD_START || sid->event_type == DICT_LOADER_EVENT_STARTED) {
        /* A specific path has just begun loading — promote it to top with spinner */
        if (sid->path && ctx->row_map) {
            GtkWidget *row = g_hash_table_lookup(ctx->row_map, sid->path);
            if (row) {
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Loading\xe2\x80\xa6");
                GtkWidget *row_spinner = g_object_get_data(G_OBJECT(row), "scan-row-spinner");
                if (row_spinner) {
                    gtk_spinner_start(GTK_SPINNER(row_spinner));
                    gtk_widget_set_visible(row_spinner, TRUE);
                }
                g_object_ref(row);
                gtk_list_box_remove(ctx->list, row);
                gtk_list_box_prepend(ctx->list, row);
                g_object_unref(row);
            }
        }
    } else if (sid->event_type == SCAN_EVENT_IMPORT_START) {
        /* File import copy phase started */
        if (sid->path && ctx->row_map) {
            GtkWidget *row = g_hash_table_lookup(ctx->row_map, sid->path);
            if (row) {
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Copying\xe2\x80\xa6");
                GtkWidget *row_spinner = g_object_get_data(G_OBJECT(row), "scan-row-spinner");
                if (row_spinner) {
                    gtk_spinner_start(GTK_SPINNER(row_spinner));
                    gtk_widget_set_visible(row_spinner, TRUE);
                }
                g_object_ref(row);
                gtk_list_box_remove(ctx->list, row);
                gtk_list_box_prepend(ctx->list, row);
                g_object_unref(row);
            }
        }
    } else if (sid->event_type == SCAN_EVENT_IMPORT_SUCCESS) {
        if (sid->path && ctx->row_map) {
            GtkWidget *row = g_hash_table_lookup(ctx->row_map, sid->path);
            if (row) {
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Copied. Waiting to load\xe2\x80\xa6");
                GtkWidget *row_spinner = g_object_get_data(G_OBJECT(row), "scan-row-spinner");
                if (row_spinner) gtk_spinner_stop(GTK_SPINNER(row_spinner));
            }
        }
    } else if (sid->event_type == SCAN_EVENT_LOAD_SUCCESS || sid->event_type == DICT_LOADER_EVENT_FINISHED) {
        /* Update row with final name and tick mark when finished. */
        if (sid->path && ctx->row_map) {
            GtkWidget *row = g_hash_table_lookup(ctx->row_map, sid->path);
            if (row) {
                if (sid->name && *sid->name) {
                    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sid->name);
                }
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sid->path);

                /* Stop and hide the per-row loading spinner */
                GtkWidget *row_spinner = g_object_get_data(G_OBJECT(row), "scan-row-spinner");
                if (row_spinner) {
                    gtk_spinner_stop(GTK_SPINNER(row_spinner));
                    gtk_widget_set_visible(row_spinner, FALSE);
                }

                GtkWidget *progress_label = g_object_get_data(G_OBJECT(row), "scan-progress-label");
                if (progress_label) gtk_widget_set_visible(progress_label, FALSE);

                if (!g_object_get_data(G_OBJECT(row), "scan-status-icon")) {
                    GtkWidget *check = gtk_image_new_from_icon_name("object-select-symbolic");
                    gtk_widget_add_css_class(check, "success");
                    adw_action_row_add_suffix(ADW_ACTION_ROW(row), check);
                    g_object_set_data(G_OBJECT(row), "scan-status-icon", check);
                }
            }
        }
        ctx->processed_count++;
        scan_context_update_processing_status(ctx);
        scan_context_finish_ui_if_ready(ctx);

    } else if (sid->event_type == DICT_LOADER_EVENT_FAILED) {
        if (sid->path && ctx->row_map) {
            gpointer val = g_hash_table_lookup(ctx->row_map, sid->path);
            if (val) {
                GtkWidget *row = val;
                if (sid->name && *sid->name) {
                    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sid->name);
                }
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Failed to load");

                /* Stop and hide the per-row loading spinner */
                GtkWidget *row_spinner = g_object_get_data(G_OBJECT(row), "scan-row-spinner");
                if (row_spinner) {
                    gtk_spinner_stop(GTK_SPINNER(row_spinner));
                    gtk_widget_set_visible(row_spinner, FALSE);
                }

                if (!g_object_get_data(G_OBJECT(row), "scan-status-icon")) {
                    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
                    adw_action_row_add_suffix(ADW_ACTION_ROW(row), icon);
                    g_object_set_data(G_OBJECT(row), "scan-status-icon", icon);
                }
            }
        }
        ctx->processed_count++;
        ctx->failed_count++;
        scan_context_update_processing_status(ctx);
        scan_context_finish_ui_if_ready(ctx);

    } else if (sid->event_type == -1) {
        ctx->scan_done_received = TRUE;
        scan_context_finish_ui_if_ready(ctx);
    }

    g_free(sid->name);
    g_free(sid->path);
    scan_context_unref(ctx);
    g_free(sid);
    return G_SOURCE_REMOVE;
}

static void scan_worker_callback(DictEntry *entry, DictLoaderEventType event, void *user_data) {
    ScanContext *ctx = user_data;
    if (!entry && event != DICT_LOADER_EVENT_FAILED) return;

    /* Copy name/path for main-thread UI update */
    char *name_copy = (entry && entry->name) ? g_strdup(entry->name) : g_strdup("(Unknown)");
    /* Use canonicalized path so progress updates match the stored key */
    char *path_copy = (entry && entry->path) ? normalize_scan_path(entry->path) : g_strdup("");

    ScanIdleData *sid = g_new0(ScanIdleData, 1);
    sid->ctx = scan_context_ref(ctx);
    sid->name = name_copy;
    sid->path = path_copy;
    sid->event_type = (int)event;
    g_idle_add(scan_idle_add_entry, sid);

    /* Free the loaded entry (we don't keep the mmap in settings dialog) */
    if (event == DICT_LOADER_EVENT_FINISHED && entry) {
        dict_entry_unref(entry);
    }
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
        st->ctx = scan_context_ref(ctx);
        st->name = g_strdup(status_buf);
        st->path = g_strdup("");
        st->event_type = (int)DICT_LOADER_EVENT_STARTED;
        g_idle_add(scan_idle_add_entry, st);

        dict_loader_scan_directory_streaming(args->dirs[i], scan_worker_callback, ctx, &ctx->generation, expected, NULL);
    }

    /* Signal completion */
    ScanIdleData *done = g_new0(ScanIdleData, 1);
    done->ctx = scan_context_ref(ctx);
    done->name = g_strdup("done");
    done->path = g_strdup("");
    done->event_type = -1;
    g_idle_add(scan_idle_add_entry, done);

    /* Free thread args */
    for (int i = 0; i < args->n_dirs; i++)
        g_free(args->dirs[i]);
    g_free(args->dirs);
    g_free(args);
    scan_context_unref(ctx);
    return NULL;
}

static gpointer file_import_thread_func(gpointer data) {
    ScanThreadArgs *args = data;
    ScanContext *ctx = args->ctx;
    GListModel *files = args->files;
    gboolean is_import = args->is_import;

    guint n = g_list_model_get_n_items(files);
    for (guint i = 0; i < n && !ctx->cancelled; i++) {
        GObject *obj = g_list_model_get_item(files, i);
        GFile *file = G_FILE(obj);
        char *path = g_file_get_path(file);
        if (!path) {
            g_object_unref(obj);
            continue;
        }

        char *name = settings_resolve_dictionary_name(path);
        
        /* Post DISCOVERED event */
        ScanIdleData *sid = g_new0(ScanIdleData, 1);
        sid->ctx = scan_context_ref(ctx);
        sid->name = g_strdup(name);
        sid->path = normalize_scan_path(path);
        sid->event_type = SCAN_EVENT_DISCOVERED;
        g_idle_add(scan_idle_add_entry, sid);

        if (is_import) {
            /* Post IMPORT_START */
            sid = g_new0(ScanIdleData, 1);
            sid->ctx = scan_context_ref(ctx);
            sid->path = normalize_scan_path(path);
            sid->event_type = SCAN_EVENT_IMPORT_START;
            g_idle_add(scan_idle_add_entry, sid);

            if (settings_import_dictionary(ctx->data->settings, path)) {
                sid = g_new0(ScanIdleData, 1);
                sid->ctx = scan_context_ref(ctx);
                sid->path = normalize_scan_path(path);
                sid->event_type = SCAN_EVENT_IMPORT_SUCCESS;
                g_idle_add(scan_idle_add_entry, sid);
            } else {
                sid = g_new0(ScanIdleData, 1);
                sid->ctx = scan_context_ref(ctx);
                sid->path = normalize_scan_path(path);
                sid->event_type = SCAN_EVENT_IMPORT_FAILURE;
                g_idle_add(scan_idle_add_entry, sid);
            }
        } else {
            /* Link: just add it */
            settings_add_dictionary(ctx->data->settings, name, path);
            sid = g_new0(ScanIdleData, 1);
            sid->ctx = scan_context_ref(ctx);
            sid->path = normalize_scan_path(path);
            sid->event_type = SCAN_EVENT_IMPORT_SUCCESS; // reusing success status for linking too
            g_idle_add(scan_idle_add_entry, sid);
        }

        g_free(name);
        g_free(path);
        g_object_unref(obj);
    }

    /* Trigger the main loader reload now that the copy/link phase is done.
     * We don't send a -1 event here because we want the dialog to stay open
     * while the main loader actually parses the files. */
    if (!ctx->cancelled && ctx->data && ctx->data->reload_callback) {
        ctx->data->reload_callback(ctx->data->user_data);
    }

    /* Free thread args */
    g_object_unref(files);
    g_free(args);
    scan_context_unref(ctx);
    return NULL;
}

void show_scan_dialog_for_files(SettingsDialogData *data, GListModel *files, gboolean is_import) {
    int n_files = (int)g_list_model_get_n_items(files);
    
    /* We reuse the folder-scan dialog UI logic. 
     * call_reload=TRUE so it persists until the main loader finishes. */
    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, is_import ? "Importing Dictionaries" : "Adding Dictionaries");
    adw_dialog_set_content_width(dialog, 720);
    adw_dialog_set_content_height(dialog, 520);
    adw_dialog_set_follows_content_size(dialog, FALSE);

    GtkWidget *toolbar_view = adw_toolbar_view_new();
    GtkWidget *header_bar = adw_header_bar_new();
    gtk_widget_add_css_class(header_bar, "flat");
    GtkWidget *title = gtk_label_new(is_import ? "Importing Dictionaries" : "Adding Dictionaries");
    gtk_widget_add_css_class(title, "title");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar), title);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);

    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(status_box), spinner);

    GtkWidget *status = gtk_label_new(is_import ? "Importing files..." : "Adding files...");
    gtk_label_set_wrap(GTK_LABEL(status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_widget_set_hexpand(status, TRUE);
    gtk_box_append(GTK_BOX(status_box), status);
    gtk_box_append(GTK_BOX(content), status_box);

    GtkWidget *description = gtk_label_new("Processing files. They will be loaded automatically when finished.");
    gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
    gtk_widget_add_css_class(description, "dim-label");
    gtk_box_append(GTK_BOX(content), description);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkListBox *list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(list), "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(list));
    gtk_box_append(GTK_BOX(content), scroller);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), content);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(footer, 12);
    gtk_widget_set_margin_bottom(footer, 12);
    gtk_widget_set_margin_start(footer, 18);
    gtk_widget_set_margin_end(footer, 18);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(footer), spacer);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_sensitive(close_btn, FALSE);
    gtk_box_append(GTK_BOX(footer), cancel_btn);
    gtk_box_append(GTK_BOX(footer), close_btn);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), footer);

    adw_dialog_set_child(dialog, toolbar_view);

    ScanContext *ctx = g_new0(ScanContext, 1);
    ctx->data = data;
    ctx->dialog = dialog;
    ctx->list = list;
    ctx->status_label = status;
    ctx->spinner = spinner;
    ctx->cancel_btn = cancel_btn;
    ctx->close_btn = close_btn;
    ctx->found_count = 0;
    ctx->total_dirs = n_files;
    ctx->call_reload = TRUE; /* Persist until loader finishes */
    ctx->ref_count = 1;

    /* Register context so main loader can notify it */
    ctx->n_scan_dirs = n_files;
    ctx->scan_dirs = g_new0(char *, n_files + 1);
    for (int i = 0; i < n_files; i++) {
        GObject *obj = g_list_model_get_item(files, (guint)i);
        char *path = g_file_get_path(G_FILE(obj));
        ctx->scan_dirs[i] = normalize_scan_path(path);
        g_free(path);
        g_object_unref(obj);
    }
    
    g_mutex_lock(&active_scan_contexts_mutex);
    active_scan_contexts = g_list_prepend(active_scan_contexts, ctx);
    g_mutex_unlock(&active_scan_contexts_mutex);

    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_scan_cancel_clicked), ctx);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_scan_close_clicked), ctx);
    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(on_scan_dialog_closed), ctx);

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(data->dialog));

    ScanThreadArgs *args = g_new0(ScanThreadArgs, 1);
    args->ctx = ctx;
    args->files = g_object_ref(files);
    args->is_import = is_import;

    scan_context_ref(ctx);
    GThread *t = g_thread_new("settings-file-import", file_import_thread_func, args);
    g_thread_unref(t);
}

static void on_scan_cancel_clicked(GtkButton *btn, ScanContext *ctx) {
    (void)btn;
    ctx->cancelled = TRUE;
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
    if (ctx->close_btn) gtk_widget_set_sensitive(ctx->close_btn, TRUE);
    if (ctx->spinner) gtk_spinner_stop(GTK_SPINNER(ctx->spinner));
    if (ctx->status_label) gtk_label_set_text(GTK_LABEL(ctx->status_label), "Canceling…");
}

static void on_scan_close_clicked(GtkButton *btn, ScanContext *ctx) {
    (void)btn;
    if (ctx->dialog) adw_dialog_close(ctx->dialog);
}

static void on_scan_dialog_closed(ScanContext *ctx) {
    if (!ctx) {
        return;
    }

    g_atomic_int_set(&ctx->closed, 1);
    if (!ctx->call_reload) {
        g_atomic_int_inc(&ctx->generation);
    }

    g_mutex_lock(&active_scan_contexts_mutex);
    active_scan_contexts = g_list_remove(active_scan_contexts, ctx);
    g_mutex_unlock(&active_scan_contexts_mutex);

    ctx->dialog = NULL;
    ctx->list = NULL;
    ctx->status_label = NULL;
    ctx->spinner = NULL;
    ctx->cancel_btn = NULL;
    ctx->close_btn = NULL;

    scan_context_unref(ctx);
}

/* Called by main loader to notify UI of discovered dictionaries. */
void settings_scan_notify(const char *name, const char *path, int event_type) {
    GPtrArray *targets = g_ptr_array_new();
    /* Precompute a canonical path used for matching / row keys */
    char *canonical_path = normalize_scan_path(path);
    g_mutex_lock(&active_scan_contexts_mutex);
    for (GList *l = active_scan_contexts; l; l = l->next) {
        ScanContext *ctx = l->data;
        if (!scan_context_is_closed(ctx) &&
            scan_context_matches_path(ctx, canonical_path, event_type)) {
            g_ptr_array_add(targets, scan_context_ref(ctx));
        }
    }
    g_mutex_unlock(&active_scan_contexts_mutex);

    for (guint i = 0; i < targets->len; i++) {
        ScanContext *ctx = g_ptr_array_index(targets, i);
        ScanIdleData *sid = g_new0(ScanIdleData, 1);
        sid->ctx = ctx;
        sid->name = name ? g_strdup(name) : g_strdup("");
        /* Use the precomputed canonical path as the row key */
        sid->path = g_strdup(canonical_path);
        sid->event_type = event_type;
        g_idle_add(scan_idle_add_entry, sid);
    }
    g_ptr_array_free(targets, TRUE);
    g_free(canonical_path);
}

/* Called by extraction code to provide progress updates for a specific
 * dictionary path. Percent should be 0..100. This posts an idle to update
 * the UI row subtitle for the matching path. */
void settings_scan_progress_notify(const char *path, int percent) {
    /* Use a static hash table to avoid redundant progress updates and expensive
     * path normalization in the caller's thread if nothing changed. */
    static GHashTable *last_percents = NULL;
    static GMutex last_percents_mutex;

    g_mutex_lock(&last_percents_mutex);
    if (!last_percents) last_percents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    
    gpointer last_val = g_hash_table_lookup(last_percents, path);
    if (last_val && GPOINTER_TO_INT(last_val) == percent) {
        g_mutex_unlock(&last_percents_mutex);
        return;
    }
    g_hash_table_replace(last_percents, g_strdup(path), GINT_TO_POINTER(percent));
    g_mutex_unlock(&last_percents_mutex);

    char *canonical_path = normalize_scan_path(path);
    GPtrArray *targets = g_ptr_array_new();
    g_mutex_lock(&active_scan_contexts_mutex);
    for (GList *l = active_scan_contexts; l; l = l->next) {
        ScanContext *ctx = l->data;
        if (!scan_context_is_closed(ctx) &&
            scan_context_matches_path(ctx, canonical_path, 0)) {
            g_ptr_array_add(targets, scan_context_ref(ctx));
        }
    }
    g_mutex_unlock(&active_scan_contexts_mutex);

    for (guint i = 0; i < targets->len; i++) {
        ScanContext *ctx = g_ptr_array_index(targets, i);
        ScanIdleData *sid = g_new0(ScanIdleData, 1);
        sid->ctx = ctx;
        sid->name = g_strdup("");
        sid->path = g_strdup(canonical_path);
        sid->event_type = -1; // Dummy type for progress notification
        sid->is_progress_update = TRUE;
        sid->progress = percent;
        g_idle_add(scan_idle_add_entry, sid);
    }
    g_ptr_array_free(targets, TRUE);
    g_free(canonical_path);
}

/* Public helper used by handlers above */
typedef struct {
    void (*cb)(void*);
    void *data;
} ReloadArgs;

static gboolean idle_reload_callback(gpointer user_data) {
    ReloadArgs *a = user_data;
    if (a->cb) a->cb(a->data);
    g_free(a);
    return G_SOURCE_REMOVE;
}

void show_scan_dialog_for_dirs(SettingsDialogData *data, char **dirs, int n_dirs, gboolean call_reload) {
    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Scanning Dictionaries");
    adw_dialog_set_content_width(dialog, 720);
    adw_dialog_set_content_height(dialog, 520);
    adw_dialog_set_follows_content_size(dialog, FALSE);

    GtkWidget *toolbar_view = adw_toolbar_view_new();

    GtkWidget *header_bar = adw_header_bar_new();
    gtk_widget_add_css_class(header_bar, "flat");
    GtkWidget *title = gtk_label_new("Scanning Dictionaries");
    gtk_widget_add_css_class(title, "title");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar), title);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);

    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_append(GTK_BOX(status_box), spinner);

    GtkWidget *status = gtk_label_new("Preparing dictionary scan…");
    gtk_label_set_wrap(GTK_LABEL(status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
    gtk_widget_set_hexpand(status, TRUE);
    gtk_box_append(GTK_BOX(status_box), status);
    gtk_box_append(GTK_BOX(content), status_box);

    GtkWidget *description = gtk_label_new("Files appear as they are discovered. The currently loading entry moves to the top.");
    gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
    gtk_widget_add_css_class(description, "dim-label");
    gtk_box_append(GTK_BOX(content), description);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);

    GtkListBox *list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(list), "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(list));
    gtk_box_append(GTK_BOX(content), scroller);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), content);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(footer, 12);
    gtk_widget_set_margin_bottom(footer, 12);
    gtk_widget_set_margin_start(footer, 18);
    gtk_widget_set_margin_end(footer, 18);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(footer), spacer);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Stop Scan");
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_sensitive(close_btn, FALSE);
    gtk_widget_remove_css_class(close_btn, "suggested-action");
    gtk_box_append(GTK_BOX(footer), cancel_btn);
    gtk_box_append(GTK_BOX(footer), close_btn);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), footer);

    adw_dialog_set_child(dialog, toolbar_view);

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
    ctx->ref_count = 1;

    /* Present dialog and either integrate with the main loader or
     * perform a local scan depending on `call_reload`. */
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_scan_cancel_clicked), ctx);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_scan_close_clicked), ctx);
    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(on_scan_dialog_closed), ctx);

    /* Present dialog and either integrate with the main loader or
     * perform a local scan depending on `call_reload`. */
    if (call_reload && settings_dialog_is_active(data) && data->reload_callback) {
        /* Copy requested dirs into context for filtering notifications (canonicalized) */
        ctx->scan_dirs = g_new0(char *, n_dirs + 1);
        for (int i = 0; i < n_dirs; i++) ctx->scan_dirs[i] = normalize_scan_path(dirs[i]);
        ctx->n_scan_dirs = n_dirs;

        /* Register context so main loader can notify it */
        g_mutex_lock(&active_scan_contexts_mutex);
        active_scan_contexts = g_list_prepend(active_scan_contexts, ctx);
        g_mutex_unlock(&active_scan_contexts_mutex);

        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(data->dialog));

        /* Kick off the main loader after a short delay so the dialog has time
         * to actually paint and appear on screen before the background thread
         * starts posting work. g_idle_add fires before GTK finishes rendering
         * the new window, so the dialog would stay invisible until all loading
         * was done. A 150ms timeout gives GTK enough frames to present it. */
        if (data->reload_callback) {
            ReloadArgs *ra = g_new0(ReloadArgs, 1);
            ra->cb = data->reload_callback;
            ra->data = data->user_data;
            g_timeout_add(150, idle_reload_callback, ra);
        }

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

    scan_context_ref(ctx);
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
    if (!d || !d->path || !settings_dialog_is_active(d->data)) return;
    if (settings_import_dictionary(d->data->settings, d->path)) {
        update_dict_list(d->data);
        char *name = g_path_get_basename(d->path);
        char *message = g_strdup_printf("Imported dictionary: %s", name);
        present_dictionary_feedback(d->data, 0, 1, 0, message);
        g_free(message);
        g_free(name);
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
    if (!settings_dialog_is_active(data)) {
        g_object_unref(chooser);
        return;
    }
    GError *error = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(chooser, result, &error);
    if (files) {
        show_scan_dialog_for_files(data, files, FALSE);
        g_object_unref(files);
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
    gtk_file_filter_add_pattern(filter, "*.slob");
    gtk_file_filter_add_pattern(filter, "*.tar.bz2");
    gtk_file_filter_add_pattern(filter, "*.tar.gz");
    gtk_file_filter_add_pattern(filter, "*.tar.xz");
    gtk_file_filter_add_pattern(filter, "*.tgz");
    gtk_file_filter_add_pattern(filter, "*.xdxf");
    gtk_file_filter_add_pattern(filter, "*.xdxf.dz");
    gtk_file_filter_add_pattern(filter, "*.zip");

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
    if (!settings_dialog_is_active(data)) {
        g_object_unref(chooser);
        return;
    }
    GError *error = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(chooser, result, &error);
    if (files) {
        show_scan_dialog_for_files(data, files, TRUE);
        g_object_unref(files);
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
    gtk_file_filter_add_pattern(filter, "*.slob");
    gtk_file_filter_add_pattern(filter, "*.tar.bz2");
    gtk_file_filter_add_pattern(filter, "*.tar.gz");
    gtk_file_filter_add_pattern(filter, "*.tar.xz");
    gtk_file_filter_add_pattern(filter, "*.tgz");
    gtk_file_filter_add_pattern(filter, "*.xdxf");
    gtk_file_filter_add_pattern(filter, "*.xdxf.dz");
    gtk_file_filter_add_pattern(filter, "*.zip");

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
    if (!settings_dialog_is_active(data)) {
        return G_SOURCE_REMOVE;
    }
    update_dict_list(data);
    update_group_list(data);
    refresh_move_buttons(data);
    return G_SOURCE_REMOVE;
}

static void on_remove_dictionary_clicked(GtkButton *btn, DictRemoveData *d) {
    (void)btn;
    if (!settings_dialog_is_active(d->data)) {
        return;
    }
    /* Save id before removal because settings_remove_dictionary frees cfg */
    char *id_copy = g_strdup(d->id);
    guint before = d->data->settings->dictionaries->len;
    char *dict_name = settings_dup_dictionary_name_by_id(d->data->settings, d->id);
    if (!dict_name) {
        dict_name = g_strdup("dictionary");
    }
    settings_remove_dictionary(d->data->settings, id_copy);
    g_hash_table_remove(d->data->group_selection_ids, id_copy);
    if (d->data->selected_dict_id &&
        strcmp(d->data->selected_dict_id, id_copy) == 0) {
        settings_dialog_set_selected_dict_id(d->data, NULL);
    }
    g_free(id_copy);
    /* Save immediately so a re-scan doesn't re-add it */
    settings_save(d->data->settings);
    if (before > d->data->settings->dictionaries->len) {
        char *message = g_strdup_printf("Removed dictionary: %s", dict_name);
        present_dictionary_feedback(d->data, 0, 0, before - d->data->settings->dictionaries->len,
                                    message);
        g_free(message);
    }
    g_free(dict_name);
    if (d->data->reload_callback) d->data->reload_callback(d->data->user_data);
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
    if (!settings_dialog_is_active(sd->data)) {
        return FALSE;
    }
    if (settings_set_dictionary_enabled_by_id(sd->data->settings, sd->dict_id, state)) {
        /* Persist immediately so restart preserves the state */
        settings_save(sd->data->settings);
        if (sd->data->soft_reload_callback) sd->data->soft_reload_callback(sd->data->user_data);
    }
    return FALSE; /* let GtkSwitch update its visual state */
}

static void on_dict_row_selected(GtkListBox *list, GtkListBoxRow *row, SettingsDialogData *data) {
    (void)list;
    if (!settings_dialog_is_active(data) || data->rebuilding_dict_list) {
        return;
    }

    if (row) {
        const char *id = g_object_get_data(G_OBJECT(row), "dict-id");
        settings_dialog_set_selected_dict_id(data, id);
    } else {
        settings_dialog_set_selected_dict_id(data, NULL);
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
    update_dictionary_overview(data);
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
    if (!settings_dialog_is_active(data)) {
        return G_SOURCE_REMOVE;
    }
    update_group_list(data);
    return G_SOURCE_REMOVE;
}

static void on_remove_group_clicked(GtkButton *btn, GroupRemoveData *d) {
    (void)btn;
    if (!settings_dialog_is_active(d->data)) {
        return;
    }
    settings_remove_group(d->data->settings, d->id);
    g_idle_add(on_remove_group_idle, d->data);
}

static void on_create_group_response(AdwAlertDialog *dialog, const char *response, GtkEntry *entry) {
    SettingsDialogData *data = g_object_get_data(G_OBJECT(dialog), "settings-data");
    if (!settings_dialog_is_active(data)) {
        return;
    }

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

static const char *dictionary_source_label(const DictConfig *cfg) {
    if (!cfg) {
        return "Unknown";
    }

    if (g_strcmp0(cfg->source, "directory") == 0) {
        return "Directory";
    }

    if (g_strcmp0(cfg->source, "imported") == 0) {
        return "Imported";
    }

    char *managed_dir = g_build_filename(g_get_user_data_dir(), "diction", "dicts", NULL);
    gboolean imported = cfg->path && g_str_has_prefix(cfg->path, managed_dir);
    g_free(managed_dir);
    return imported ? "Imported" : "Manual";
}

static const char *dictionary_source_icon(const DictConfig *cfg) {
    if (!cfg) {
        return "help-about-symbolic";
    }

    if (g_strcmp0(cfg->source, "directory") == 0) {
        return "folder-visiting-symbolic";
    }

    if (g_strcmp0(cfg->source, "imported") == 0) {
        return "folder-download-symbolic";
    }

    char *managed_dir = g_build_filename(g_get_user_data_dir(), "diction", "dicts", NULL);
    gboolean imported = cfg->path && g_str_has_prefix(cfg->path, managed_dir);
    g_free(managed_dir);
    return imported ? "folder-download-symbolic" : "document-open-symbolic";
}

static GtkWidget *create_source_badge(const DictConfig *cfg) {
    GtkWidget *label = gtk_label_new(dictionary_source_label(cfg));
    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    return label;
}

static void update_dir_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->dir_list))))
        gtk_list_box_remove(data->dir_list, child);

    if (data->settings->dictionary_dirs->len == 0) {
        GtkWidget *row = new_plain_action_row();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No directories configured");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Add one or more folders containing dictionaries");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(data->dir_list, GTK_WIDGET(row));
        update_dictionary_overview(data);
        return;
    }

    for (guint i = 0; i < data->settings->dictionary_dirs->len; i++) {
        const char *path = g_ptr_array_index(data->settings->dictionary_dirs, i);
        char *name = g_path_get_basename(path);

        GtkWidget *row = new_plain_action_row();
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

    update_dictionary_overview(data);
}

static void update_dict_list(SettingsDialogData *data) {
    char *previous_selection = NULL;
    GtkListBoxRow *selected_row = NULL;
    GtkWidget *child;

    if (!settings_dialog_is_active(data)) {
        return;
    }

    previous_selection = g_strdup(data->selected_dict_id);
    data->rebuilding_dict_list = TRUE;
    settings_dialog_set_selected_dict_id(data, NULL);
    gtk_list_box_unselect_all(data->dict_list);

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->dict_list))))
        gtk_list_box_remove(data->dict_list, child);

    if (data->settings->dictionaries->len == 0) {
        GtkWidget *row = new_plain_action_row();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No dictionaries available");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
            "Add a dictionary file or rescan configured directories.");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(data->dict_list, GTK_WIDGET(row));
        data->rebuilding_dict_list = FALSE;
        g_free(previous_selection);
        refresh_move_buttons(data);
        return;
    }

    for (guint i = 0; i < data->settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(data->settings->dictionaries, i);

        GtkWidget *row = new_plain_action_row();
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

        GtkWidget *source_icon = gtk_image_new_from_icon_name(dictionary_source_icon(cfg));
        gtk_widget_set_valign(source_icon, GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), source_icon);

        /* Enable/disable switch — use id string, not raw cfg* */
        GtkWidget *switch_widget = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(switch_widget), cfg->enabled);
        gtk_widget_set_valign(switch_widget, GTK_ALIGN_CENTER);

        DictSwitchData *sd = g_new(DictSwitchData, 1);
        sd->data    = data;
        sd->dict_id = g_strdup(cfg->id);
        g_signal_connect_data(switch_widget, "state-set", G_CALLBACK(on_dict_switch_state),
            sd, dict_switch_data_destroy, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), create_source_badge(cfg));
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
        char *data_root = g_build_filename(g_get_user_data_dir(), "diction", "dicts", NULL);
        gboolean already_managed = cfg->path && g_str_has_prefix(cfg->path, data_root);
        g_free(data_root);
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

        if (previous_selection && strcmp(previous_selection, cfg->id) == 0) {
            selected_row = GTK_LIST_BOX_ROW(row);
        }
    }

    if (selected_row) {
        gtk_list_box_select_row(data->dict_list, selected_row);
        settings_dialog_set_selected_dict_id(data, previous_selection);
    }

    data->rebuilding_dict_list = FALSE;
    g_free(previous_selection);
    refresh_move_buttons(data);
    update_dictionary_overview(data);
}

static void update_group_list(SettingsDialogData *data) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->group_list))))
        gtk_list_box_remove(data->group_list, child);

    if (data->settings->dictionary_groups->len == 0) {
        GtkWidget *row = new_plain_action_row();
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

        GtkWidget *row = new_plain_action_row();
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

    update_dictionary_overview(data);
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
    guint idx = adw_combo_row_get_selected(row);
    GPtrArray *all_names = g_object_get_data(G_OBJECT(row), "theme-names-array");
    if (!all_names || idx >= all_names->len) return;
    
    g_free(data->settings->color_theme);
    data->settings->color_theme = g_strdup(g_ptr_array_index(all_names, idx));
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

static void on_system_switch_action(GtkSwitch *sw, GParamSpec *pspec, SettingsDialogData *data) {
    (void)pspec;
    gboolean active = gtk_switch_get_active(sw);
    const char *key = g_object_get_data(G_OBJECT(sw), "setting-key");
    if (!key) return;

    if (strcmp(key, "tray_icon_enabled") == 0) {
        data->settings->tray_icon_enabled = active;
    } else if (strcmp(key, "close_to_tray") == 0) {
        data->settings->close_to_tray = active;
    } else if (strcmp(key, "scan_popup_enabled") == 0) {
        data->settings->scan_popup_enabled = active;
    } else if (strcmp(key, "scan_selection_enabled") == 0) {
        data->settings->scan_selection_enabled = active;
    } else if (strcmp(key, "scan_clipboard_enabled") == 0) {
        data->settings->scan_clipboard_enabled = active;
    }
    
    settings_save(data->settings);

    if (data->soft_reload_callback) {
        data->soft_reload_callback(data->user_data);
    }
}

static void on_scan_modifier_row_changed(AdwComboRow *row, GParamSpec *pspec, SettingsDialogData *data) {
    (void)pspec;
    const char *values[] = {"none", "ctrl", "alt", "meta"};
    guint idx = adw_combo_row_get_selected(row);
    if (idx >= G_N_ELEMENTS(values)) return;

    g_free(data->settings->scan_modifier_key);
    data->settings->scan_modifier_key = g_strdup(values[idx]);
    settings_save(data->settings);

    if (data->soft_reload_callback) {
        data->soft_reload_callback(data->user_data);
    }
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
    if (!data) {
        return;
    }

    data->closed = TRUE;
    settings_save(data->settings);
    /* Do NOT call reload_callback here — it's only needed on explicit Rescan.
       Calling it unconditionally on close caused double-reload jank. */
    if (data->banner_timeout_id != 0) {
        g_source_remove(data->banner_timeout_id);
        data->banner_timeout_id = 0;
    }
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
    g_object_set_data_full(G_OBJECT(dialog), "sdd", data, settings_dialog_data_free);

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

    GtkStringList *color_model = gtk_string_list_new(NULL);
    const char *hardcoded_titles[] = {"Default", "Solarized", "Dracula", "Nord", "Gruvbox", "Monokai", "Material", "Ocean", "Forest", "Sepia"};
    const char *hardcoded_names[] = {"default", "solarized", "dracula", "nord", "gruvbox", "monokai", "material", "ocean", "forest", "sepia"};
    int n_hardcoded = 10;
    
    GPtrArray *all_theme_names = g_ptr_array_new_with_free_func(g_free);
    for (int i=0; i<n_hardcoded; i++) {
        gtk_string_list_append(color_model, hardcoded_titles[i]);
        g_ptr_array_add(all_theme_names, g_strdup(hardcoded_names[i]));
    }
    
    json_theme_manager_init();
    int n_json_themes = json_theme_get_count();
    for (int i=0; i<n_json_themes; i++) {
        const char *name = json_theme_get_name(i);
        char *title = g_strdup_printf("%s (Custom)", name);
        gtk_string_list_append(color_model, title);
        g_free(title);
        g_ptr_array_add(all_theme_names, g_strdup(name));
    }

    adw_combo_row_set_model(color_theme_row, G_LIST_MODEL(color_model));
    g_object_set_data_full(G_OBJECT(color_theme_row), "theme-names-array", all_theme_names, (GDestroyNotify)g_ptr_array_unref);
    g_object_unref(color_model);

    int color_idx = 0;
    for (guint i=0; i<all_theme_names->len; i++) {
        if (g_strcmp0(settings->color_theme, g_ptr_array_index(all_theme_names, i)) == 0) { 
            color_idx = i; 
            break; 
        }
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
       TAB 2 — System Integration
       ============================================================ */
    AdwPreferencesPage *system_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(system_page, "System");
    adw_preferences_page_set_icon_name(system_page, "preferences-system-symbolic");
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), system_page);

    AdwPreferencesGroup *tray_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(tray_group, "System Tray");
    adw_preferences_group_set_description(tray_group, "Background operation options");
    adw_preferences_page_add(system_page, tray_group);

    AdwSwitchRow *tray_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(tray_row), "Enable System Tray Icon");
    adw_switch_row_set_active(tray_row, settings->tray_icon_enabled);
    GtkWidget *tray_switch = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(tray_row));
    g_object_set_data(G_OBJECT(tray_switch), "setting-key", "tray_icon_enabled");
    g_signal_connect(tray_switch, "notify::active", G_CALLBACK(on_system_switch_action), data);
    adw_preferences_group_add(tray_group, GTK_WIDGET(tray_row));

    AdwSwitchRow *close_to_tray_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(close_to_tray_row), "Close Window to Tray");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(close_to_tray_row), "Closing main window hides it instead of quitting");
    adw_switch_row_set_active(close_to_tray_row, settings->close_to_tray);
    GtkWidget *close_to_tray_switch = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(close_to_tray_row));
    g_object_set_data(G_OBJECT(close_to_tray_switch), "setting-key", "close_to_tray");
    g_signal_connect(close_to_tray_switch, "notify::active", G_CALLBACK(on_system_switch_action), data);
    adw_preferences_group_add(tray_group, GTK_WIDGET(close_to_tray_row));

    g_object_bind_property(tray_row, "active", close_to_tray_row, "sensitive", G_BINDING_SYNC_CREATE);

    AdwPreferencesGroup *popup_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(popup_group, "Clipboard Scan Popup");
    adw_preferences_group_set_description(popup_group, "Look up definitions when selecting text in other applications");
    adw_preferences_page_add(system_page, popup_group);

    AdwSwitchRow *scan_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scan_row), "Enable Scan Popup");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(scan_row), "Look up selected or copied words in a popup");
    adw_switch_row_set_active(scan_row, settings->scan_popup_enabled);
    GtkWidget *scan_switch = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(scan_row));
    g_object_set_data(G_OBJECT(scan_switch), "setting-key", "scan_popup_enabled");
    g_signal_connect(scan_switch, "notify::active", G_CALLBACK(on_system_switch_action), data);
    adw_preferences_group_add(popup_group, GTK_WIDGET(scan_row));

    AdwSwitchRow *scan_selection_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scan_selection_row), "Scan Selected Text");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(scan_selection_row), "Watch PRIMARY selection changes");
    adw_switch_row_set_active(scan_selection_row, settings->scan_selection_enabled);
    GtkWidget *scan_selection_switch = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(scan_selection_row));
    g_object_set_data(G_OBJECT(scan_selection_switch), "setting-key", "scan_selection_enabled");
    g_signal_connect(scan_selection_switch, "notify::active", G_CALLBACK(on_system_switch_action), data);
    adw_preferences_group_add(popup_group, GTK_WIDGET(scan_selection_row));

    AdwSwitchRow *scan_clipboard_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scan_clipboard_row), "Scan Copied Text");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(scan_clipboard_row), "Watch regular clipboard changes, including Ctrl+C");
    adw_switch_row_set_active(scan_clipboard_row, settings->scan_clipboard_enabled);
    GtkWidget *scan_clipboard_switch = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(scan_clipboard_row));
    g_object_set_data(G_OBJECT(scan_clipboard_switch), "setting-key", "scan_clipboard_enabled");
    g_signal_connect(scan_clipboard_switch, "notify::active", G_CALLBACK(on_system_switch_action), data);
    adw_preferences_group_add(popup_group, GTK_WIDGET(scan_clipboard_row));

    AdwComboRow *scan_modifier_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scan_modifier_row), "Scan Modifier");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(scan_modifier_row), "Automatic popup only runs while this key is held");
    GtkStringList *scan_modifier_model = gtk_string_list_new((const char *[]){
        "No Modifier", "Ctrl", "Alt", "Meta / Super", NULL});
    adw_combo_row_set_model(scan_modifier_row, G_LIST_MODEL(scan_modifier_model));
    g_object_unref(scan_modifier_model);
    guint scan_modifier_idx = 0;
    if (g_strcmp0(settings->scan_modifier_key, "ctrl") == 0) scan_modifier_idx = 1;
    else if (g_strcmp0(settings->scan_modifier_key, "alt") == 0) scan_modifier_idx = 2;
    else if (g_strcmp0(settings->scan_modifier_key, "meta") == 0) scan_modifier_idx = 3;
    adw_combo_row_set_selected(scan_modifier_row, scan_modifier_idx);
    g_signal_connect(scan_modifier_row, "notify::selected", G_CALLBACK(on_scan_modifier_row_changed), data);
    adw_preferences_group_add(popup_group, GTK_WIDGET(scan_modifier_row));

    AdwPreferencesGroup *shortcut_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(shortcut_group, "Global Shortcut");
    adw_preferences_page_add(system_page, shortcut_group);

    GtkWidget *shortcut_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(shortcut_row), "Scan Selected Text");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(shortcut_row), "Super+Alt+L, requires XDG Desktop Portal");
    
    AdwButtonRow *shortcut_btn_row = ADW_BUTTON_ROW(adw_button_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(shortcut_btn_row), "Configure Global Shortcut");
    // Handled in main.c during portal initialization as there's no native bind UI here
    adw_preferences_group_add(shortcut_group, GTK_WIDGET(shortcut_row));

    /* ============================================================
       Search group — FTS persistent index
       ============================================================ */
    AdwPreferencesGroup *search_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(search_group, "Search");
    adw_preferences_group_set_description(search_group,
        "Full text search indexes definitions on disk for fast content lookup");
    adw_preferences_page_add(system_page, search_group);

    AdwSwitchRow *fts_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(fts_row),
                                  "Enable Full Text Search Index");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(fts_row),
        "Creates per-dictionary SQLite indexes in ~/.cache/diction/fts/");
    adw_switch_row_set_active(fts_row, settings->fts_enabled);
    GtkWidget *fts_switch = adw_action_row_get_activatable_widget(ADW_ACTION_ROW(fts_row));
    g_signal_connect(fts_switch, "notify::active",
                     G_CALLBACK(on_fts_switch_toggled), data);
    adw_preferences_group_add(search_group, GTK_WIDGET(fts_row));

    AdwButtonRow *fts_manage_row = ADW_BUTTON_ROW(adw_button_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(fts_manage_row),
                                  "Build / Manage Indexes…");
    g_signal_connect(fts_manage_row, "activated",
                     G_CALLBACK(on_fts_manage_clicked), data);
    adw_preferences_group_add(search_group, GTK_WIDGET(fts_manage_row));
    g_object_bind_property(fts_row, "active", fts_manage_row, "sensitive",
                           G_BINDING_SYNC_CREATE);

    /* ============================================================
       TAB 2 — Dictionaries
       ============================================================ */
    AdwPreferencesPage *dict_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(dict_page, "Dictionaries");
    adw_preferences_page_set_icon_name(dict_page, "accessories-dictionary-symbolic");
    adw_preferences_page_set_description(dict_page,
        "Manage scanned folders, one-off dictionary files, and imported copies.");
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), dict_page);

    data->dict_banner = ADW_BANNER(adw_banner_new("Dictionary changes will appear here."));
    adw_banner_set_use_markup(data->dict_banner, FALSE);
    adw_banner_set_revealed(data->dict_banner, FALSE);
    adw_preferences_page_set_banner(dict_page, data->dict_banner);

    AdwPreferencesGroup *overview_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(overview_group, "Overview");
    adw_preferences_group_set_description(overview_group,
        "Keep an eye on your library size, current selection, and session changes.");
    adw_preferences_page_add(dict_page, overview_group);

    data->dict_library_row = ADW_ACTION_ROW(new_plain_action_row());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(data->dict_library_row), "Library");
    adw_preferences_group_add(overview_group, GTK_WIDGET(data->dict_library_row));

    data->dict_selection_row = ADW_ACTION_ROW(new_plain_action_row());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(data->dict_selection_row), "Selection");
    adw_preferences_group_add(overview_group, GTK_WIDGET(data->dict_selection_row));

    data->dict_activity_row = ADW_ACTION_ROW(new_plain_action_row());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(data->dict_activity_row), "Session Changes");
    adw_preferences_group_add(overview_group, GTK_WIDGET(data->dict_activity_row));

    /* --- Directory group --- */
    AdwPreferencesGroup *dir_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(dir_group, "Scanned Folders");
    adw_preferences_group_set_description(dir_group,
        "Folders are watched in bulk and all supported dictionaries inside them are loaded automatically.");
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
    adw_preferences_group_set_title(dict_group, "Library");
    adw_preferences_group_set_description(dict_group,
        "Click a row to change priority. Tick checkboxes to create groups. Import copies files into Diction-managed storage.");
    adw_preferences_page_add(dict_page, dict_group);

    data->dict_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(data->dict_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(data->dict_list), "boxed-list");
    g_signal_connect(data->dict_list, "row-selected", G_CALLBACK(on_dict_row_selected), data);
    adw_preferences_group_add(dict_group, GTK_WIDGET(data->dict_list));

    GtkWidget *reorder_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(reorder_box, TRUE);

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

    GtkWidget *move_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(move_box, "linked");
    gtk_box_append(GTK_BOX(move_box), data->move_up_btn);
    gtk_box_append(GTK_BOX(move_box), data->move_down_btn);

    gtk_box_append(GTK_BOX(reorder_box), move_box);
    GtkWidget *reorder_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(reorder_spacer, TRUE);
    gtk_box_append(GTK_BOX(reorder_box), reorder_spacer);
    gtk_box_append(GTK_BOX(reorder_box), data->create_group_btn);
    adw_preferences_group_add(dict_group, reorder_box);

    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add_file_btn = gtk_button_new_with_label("Add File…");
    g_signal_connect(add_file_btn, "clicked", G_CALLBACK(on_add_dictionary_file), data);
    gtk_box_append(GTK_BOX(action_box), add_file_btn);

    GtkWidget *import_btn = gtk_button_new_with_label("Import into Diction…");
    gtk_widget_add_css_class(import_btn, "suggested-action");
    g_signal_connect(import_btn, "clicked", G_CALLBACK(on_import_dictionary_files), data);
    gtk_box_append(GTK_BOX(action_box), import_btn);
    adw_preferences_group_add(dict_group, action_box);

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
