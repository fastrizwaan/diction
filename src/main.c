#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <adwaita.h>
#include "langpair.h"
#include <webkit/webkit.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "dict-mmap.h"
#include "dict-loader.h"
#include "dict-cache.h"
#include "dict-render.h"
#include "global-shortcut.h"
#include "settings.h"
#include "audio-playback.h"
#include "scan-popup.h"
#include "search-utils.h"
#include "startup-splash.h"
#include "tray-icon.h"

static DictEntry *all_dicts = NULL;
static DictEntry *active_entry = NULL;
static AdwTabView *tab_view = NULL;

static volatile gint loader_generation = 0;
static GMutex loader_cancel_mutex;
static GCancellable *loader_cancellable = NULL;
static GMutex dict_loader_mutex;


static WebKitWebView *get_web_view_from_scroll(GtkWidget *scroll) {
    if (!scroll || !GTK_IS_SCROLLED_WINDOW(scroll)) return NULL;
    WebKitWebView *stored = g_object_get_data(G_OBJECT(scroll), "web-view");
    if (stored) return stored;
    GtkWidget *child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
    if (GTK_IS_VIEWPORT(child)) {
        child = gtk_viewport_get_child(GTK_VIEWPORT(child));
    }
    return WEBKIT_IS_WEB_VIEW(child) ? WEBKIT_WEB_VIEW(child) : NULL;
}

static WebKitWebView *get_current_web_view(void) {
    if (!tab_view) return NULL;
    AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
    if (!page) return NULL;
    GtkWidget *scroll = adw_tab_page_get_child(page);
    return get_web_view_from_scroll(scroll);
}
#define web_view get_current_web_view()

static AdwStyleManager *style_manager = NULL;
static GtkEntry *search_entry = NULL;
static GtkStack *search_stack = NULL;
static void populate_search_sidebar(const char *query);
static void populate_search_sidebar_with_mode(const char *query, gboolean force_fts);

static AdwTabPage *create_new_tab(const char *title, gboolean select_it);
static void on_tab_selected(AdwTabView *view, GParamSpec *pspec, gpointer user_data);

static GPtrArray *split_headword_variants(const char *headword);
static GtkButton *search_button = NULL;
static GtkLabel *search_button_label = NULL;
static GtkImage *search_mode_icon = NULL;
static GtkMenuButton *search_scope_button = NULL;
static GtkLabel *search_scope_button_label = NULL;
static char *last_search_query = NULL;
static AppSettings *app_settings = NULL;
static char *active_scope_id = NULL;
static GPtrArray *history_words = NULL;
static GPtrArray *favorite_words = NULL;
/* static nav history moved to tab locals */
static GtkRevealer *find_revealer = NULL;
static GtkSearchEntry *find_bar_entry = NULL;
static GtkLabel *find_status_label = NULL;

typedef struct {
    char *view_word;
    char *search_query;
    gboolean search_is_fts;
} NavHistoryItem;

static void nav_history_item_free(gpointer data) {
    NavHistoryItem *item = data;
    if (item) {
        g_free(item->view_word);
        g_free(item->search_query);
        g_free(item);
    }
}

/* static int nav_history_index = -1; */
static GtkWidget *nav_back_btn = NULL;
static GtkWidget *nav_forward_btn = NULL;
static GSimpleAction *full_text_search_toggle_action = NULL;
static GSimpleAction *search_scope_action = NULL;
static guint search_execute_source_id = 0;
static GtkStringList *related_string_list = NULL;
static GtkSingleSelection *related_selection_model = NULL;
static GtkListView *related_list_view = NULL;
static guint related_activated_pos = GTK_INVALID_LIST_POSITION;
static GPtrArray *related_row_payloads = NULL;
static GHashTable *dictionary_dir_monitors = NULL;
static GHashTable *dictionary_root_parent_monitors = NULL;
static guint dictionary_watch_reload_source_id = 0;
static gboolean force_directory_rescan_requested = FALSE;
static WebKitUserContentManager *font_ucm = NULL;       /* shared across web views */
static WebKitUserStyleSheet *font_user_stylesheet = NULL; /* current injected font CSS */
static GtkWindow *main_window = NULL;
static void app_show_window(void);
static gboolean dictionary_loading_in_progress = FALSE;
static gint64 rescan_suppress_until = 0;
static gboolean startup_random_word_pending = FALSE;

typedef enum {
    SIDEBAR_ROW_HINT = 0,
    SIDEBAR_ROW_WORD,
    SIDEBAR_ROW_GROUP,
    SIDEBAR_ROW_DICT
} SidebarRowType;

typedef struct {
    SidebarRowType type;
    char *title;
    char *subtitle;
    char *scope_id;
    char *icon_path;
    DictEntry *dict_entry;
} SidebarRowPayload;

typedef struct {
    GtkStringList *string_list;
    GtkSingleSelection *selection_model;
    GtkListView *list_view;
    GPtrArray *payloads;
    guint activated_pos;
} SidebarListView;

static SidebarListView dict_sidebar = {0};
static SidebarListView history_sidebar = {0};
static SidebarListView favorites_sidebar = {0};

static GtkCssProvider *dynamic_theme_provider = NULL;

/* Request cancellation of the current loader generation (called from UI). */
void request_loader_cancel(void) {
    g_atomic_int_inc(&loader_generation);
    g_mutex_lock(&loader_cancel_mutex);
    if (loader_cancellable) {
        g_cancellable_cancel(loader_cancellable);
    }
    g_mutex_unlock(&loader_cancel_mutex);
}

/* Safely produce a markup-escaped UTF-8 string from possibly-binary input.
 * If `len` >= 0, the input is treated as a byte buffer of that length;
 * otherwise it is treated as a NUL-terminated string. The returned string
 * is newly allocated and must be freed by the caller. */
static char *safe_markup_escape_n(const char *buf, gssize len) {
    char *tmp = NULL;
    if (len < 0) {
        tmp = g_strdup(buf ? buf : "");
    } else {
        tmp = g_strndup(buf ? buf : "", len);
    }
    char *valid = g_utf8_make_valid(tmp, -1);
    g_free(tmp);
    char *escaped = g_markup_escape_text(valid, -1);
    g_free(valid);
    return escaped;
}

static void render_idle_page_to_webview(WebKitWebView *target_wv,
                                        const char *title,
                                        const char *message_html) {
    if (!target_wv) return;

    int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
    dsl_theme_palette palette;
    dict_render_get_theme_palette(
        (app_settings && app_settings->color_theme) ? app_settings->color_theme : "default",
        dark_mode,
        &palette);

    char *escaped_title = safe_markup_escape_n(title ? title : "Diction", -1);
    char *html = g_strdup_printf(
        "<html><body style='font-family: sans-serif; background: %s; color: %s; margin: 0;'>"
        "<div style='min-height: 100vh; box-sizing: border-box; display: flex; align-items: center; justify-content: center; padding: 24px;'>"
        "<div style='max-width: 40rem; width: 100%%; text-align: center; padding: 24px 28px; border: 1px solid %s; border-radius: 16px; background: %s;'>"
        "<h2 style='margin: 0 0 12px 0; color: %s;'>%s</h2>"
        "<div style='opacity: 0.78; line-height: 1.6;'>%s</div>"
        "</div></div></body></html>",
        palette.bg,
        palette.fg,
        palette.border,
        palette.bg,
        palette.heading,
        escaped_title,
        message_html ? message_html : "");
    webkit_web_view_load_html(target_wv, html, "file:///");
    g_free(html);
    g_free(escaped_title);
}




#define HISTORY_FILE_NAME "history.json"
#define FAVORITES_FILE_NAME "favorites.json"
static void populate_dict_sidebar(void);      // forward declaration
static gboolean start_async_dict_loading(gboolean discover_from_dirs);   // forward declaration
static void on_search_changed(GtkEditable *entry, gpointer user_data); // forward declaration
static void on_random_clicked(GtkButton *btn, gpointer user_data);
static void maybe_show_startup_random_word(void);
static void refresh_search_results(void);
static void render_idle_page_to_webview(WebKitWebView *target_wv,
                                        const char *title,
                                        const char *message_html);
static void render_query_to_webview(const char *query_raw, WebKitWebView *target_wv, gboolean push_history);
static void update_listview_hw_selected(GtkListView *list_view, guint activated_pos);
static void populate_search_sidebar(const char *query);
static void execute_search_now(void);
static void execute_search_now_for_query(const char *query_raw, gboolean push_history);
static void activate_dictionary_entry(DictEntry *e);
static void set_active_entry(DictEntry *new_entry);
static void finalize_dictionary_loading(gboolean allow_random_word, gboolean sync_settings_from_loaded);
static gboolean on_dict_loaded_idle(gpointer user_data);
static void apply_font_to_webview(void *user_data);
static void reveal_search_entry(gboolean select_text);
static gboolean current_tab_is_full_text_search(void);
static gboolean query_requests_full_text_search(const char *query_raw, gboolean preferred_fts);
static void set_tab_full_text_search(AdwTabPage *page, gboolean is_fts);
static void update_search_mode_visuals(gboolean is_fts);
static void update_search_scope_button_label(void);
static void rebuild_search_scope_menu(void);
static void set_active_scope(const char *scope_id, gboolean refresh_results);
static void apply_fts_highlight_to_web_view(WebKitWebView *wv, const char *query);
static void queue_fts_highlight_for_web_view(WebKitWebView *wv, const char *query);

#define BUCKET_COUNT 6
#define MAX_FTS_DICTIONARIES 30
#define MAX_EXACT_RENDERED_MATCHES 24

typedef struct {
    char *query;
    char *query_key;
    char *query_compact_key;
    guint query_len;
    guint query_compact_len;
    gboolean skip_fast_prefilter;
    GHashTable *seen_words;
    GPtrArray *search_entries;
    guint scoped_dict_count;
    guint searched_dict_count;
    gboolean fts_limited;
    guint current_entry_index;
    DictEntry *current_dict;
    size_t current_dict_count;
    size_t current_pos;  /* position in flat index */
    gboolean has_current_pos;
    gboolean list_started;
    gboolean prefix_only;
    volatile gint cancelled;
    gint ref_count;
    GPtrArray *global_bucket_labels[BUCKET_COUNT];
    GPtrArray *global_bucket_payloads[BUCKET_COUNT];
    gboolean is_fts;
    GRegex *fts_regex;
} SidebarSearchState;

static SidebarSearchState *sidebar_search_state = NULL;
static char *fts_highlight_query = NULL;

typedef enum {
    RELATED_ROW_HINT = 0,
    RELATED_ROW_CANDIDATE
} RelatedRowType;

typedef struct {
    RelatedRowType type;
    char *word;
    char *sort_key;
    double fuzzy_score;
} RelatedRowPayload;

static void related_row_payload_free(RelatedRowPayload *payload) {
    if (!payload) {
        return;
    }
    g_free(payload->word);
    g_free(payload->sort_key);
    g_free(payload);
}

static void sidebar_row_payload_free(SidebarRowPayload *payload) {
    if (!payload) {
        return;
    }
    g_free(payload->title);
    g_free(payload->subtitle);
    g_free(payload->scope_id);
    g_free(payload->icon_path);
    if (payload->dict_entry) {
        dict_entry_unref(payload->dict_entry);
    }
    g_free(payload);
}

static char *get_app_config_file_path(const char *filename) {
    char *dir = g_build_filename(g_get_user_config_dir(), "diction", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, filename, NULL);
    g_free(dir);
    return path;
}

static void free_word_list(GPtrArray **list_ptr) {
    if (list_ptr && *list_ptr) {
        g_ptr_array_free(*list_ptr, TRUE);
        *list_ptr = NULL;
    }
}

static SidebarSearchState *sidebar_search_state_ref(SidebarSearchState *state) {
    if (state) g_atomic_int_inc(&state->ref_count);
    return state;
}

static void sidebar_search_state_unref(SidebarSearchState *state) {
    if (!state) return;
    if (!g_atomic_int_dec_and_test(&state->ref_count)) return;
    
    g_free(state->query);
    g_free(state->query_key);
    g_free(state->query_compact_key);
    if (state->seen_words) {
        g_hash_table_unref(state->seen_words);
    }
    for (int i = 0; i < BUCKET_COUNT; i++) {
        if (state->global_bucket_labels[i]) {
            for (guint j = 0; j < state->global_bucket_labels[i]->len; j++) {
                g_free(g_ptr_array_index(state->global_bucket_labels[i], j));
            }
            g_ptr_array_free(state->global_bucket_labels[i], TRUE);
        }
        if (state->global_bucket_payloads[i]) {
            for (guint j = 0; j < state->global_bucket_payloads[i]->len; j++) {
                related_row_payload_free(g_ptr_array_index(state->global_bucket_payloads[i], j));
            }
            g_ptr_array_free(state->global_bucket_payloads[i], TRUE);
        }
    }
    if (state->fts_regex) {
        g_regex_unref(state->fts_regex);
    }
    if (state->search_entries) {
        g_ptr_array_free(state->search_entries, TRUE);
    }
    if (state->current_dict) dict_entry_unref(state->current_dict);
    g_free(state);
}

static gboolean headword_variants_overlap_ci(const char *left, const char *right) {
    if (!left || !right) {
        return FALSE;
    }

    GPtrArray *left_variants = split_headword_variants(left);
    GPtrArray *right_variants = split_headword_variants(right);
    gboolean found = FALSE;

    for (guint i = 0; i < left_variants->len && !found; i++) {
        const char *l = g_ptr_array_index(left_variants, i);
        for (guint j = 0; j < right_variants->len; j++) {
            const char *r = g_ptr_array_index(right_variants, j);
            if (g_ascii_strcasecmp(l, r) == 0) {
                found = TRUE;
                break;
            }
        }
    }

    g_ptr_array_free(left_variants, TRUE);
    g_ptr_array_free(right_variants, TRUE);
    return found;
}

static gint word_list_find_ci(GPtrArray *list, const char *word) {
    if (!list || !word) {
        return -1;
    }
    for (guint i = 0; i < list->len; i++) {
        const char *item = g_ptr_array_index(list, i);
        if (headword_variants_overlap_ci(item, word)) {
            return (gint)i;
        }
    }
    return -1;
}

static gboolean word_list_contains_ci(GPtrArray *list, const char *word) {
    return word_list_find_ci(list, word) >= 0;
}

static GPtrArray *load_word_list(const char *filename, guint limit) {
    GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
    char *path = get_app_config_file_path(filename);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return words;
    }

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_file(parser, path, &error)) {
        if (error) {
            g_error_free(error);
        }
        g_object_unref(parser);
        g_free(path);
        return words;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *array = json_node_get_array(root);
        for (guint i = 0; i < json_array_get_length(array); i++) {
            char *word = sanitize_user_word(json_array_get_string_element(array, i));
            if (!word) {
                continue;
            }
            GPtrArray *variants = split_headword_variants(word);
            for (guint j = 0; j < variants->len; j++) {
                const char *variant = g_ptr_array_index(variants, j);
                if (word_list_contains_ci(words, variant)) {
                    continue;
                }
                g_ptr_array_add(words, g_strdup(variant));
                if (limit > 0 && words->len >= limit) {
                    break;
                }
            }
            g_ptr_array_free(variants, TRUE);
            g_free(word);
            if (limit > 0 && words->len >= limit) {
                break;
            }
        }
    }

    g_object_unref(parser);
    g_free(path);
    return words;
}

static void save_word_list(GPtrArray *words, const char *filename) {
    if (!words) {
        return;
    }

    char *path = get_app_config_file_path(filename);
    JsonArray *array = json_array_new();
    for (guint i = 0; i < words->len; i++) {
        json_array_add_string_element(array, g_ptr_array_index(words, i));
    }

    JsonNode *root = json_node_alloc();
    json_node_init_array(root, array);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, root);
    json_generator_to_file(gen, path, NULL);
    g_object_unref(gen);
    json_node_free(root);
    json_array_unref(array);
    g_free(path);
}

static void clear_related_rows(void) {
    if (related_row_payloads) {
        g_ptr_array_set_size(related_row_payloads, 0);
    }
    if (GTK_IS_STRING_LIST(related_string_list)) {
        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(related_string_list));
        gtk_string_list_splice(related_string_list, 0, n_items, NULL);
    }
    related_activated_pos = GTK_INVALID_LIST_POSITION;
}

static void set_related_rows(GPtrArray *labels, GPtrArray *payloads) {
    char *selected_text = NULL;
    if (related_selection_model) {
        guint pos = gtk_single_selection_get_selected(related_selection_model);
        if (pos != GTK_INVALID_LIST_POSITION) {
            GtkStringObject *obj = g_list_model_get_item(G_LIST_MODEL(related_string_list), pos);
            if (obj) selected_text = g_strdup(gtk_string_object_get_string(obj));
        }
    }

    clear_related_rows();
    if (!labels || labels->len == 0 || !GTK_IS_STRING_LIST(related_string_list) || !related_row_payloads) {
        g_free(selected_text);
        return;
    }

    char **items = g_new0(char *, labels->len + 1);
    for (guint i = 0; i < labels->len; i++) {
        items[i] = g_ptr_array_index(labels, i);
    }
    gtk_string_list_splice(related_string_list, 0, 0, (const char * const *)items);
    g_free(items);

    for (guint i = 0; i < payloads->len; i++) {
        g_ptr_array_add(related_row_payloads, g_ptr_array_index(payloads, i));
    }
    g_ptr_array_set_free_func(payloads, NULL);
    g_ptr_array_set_size(payloads, 0);

    /* Restore selection and hw-selected highlight if the word is still in the new list */
    if (selected_text && related_selection_model) {
        for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(related_string_list)); i++) {
            GtkStringObject *obj = g_list_model_get_item(G_LIST_MODEL(related_string_list), i);
            if (obj && g_strcmp0(gtk_string_object_get_string(obj), selected_text) == 0) {
                gtk_single_selection_set_selected(related_selection_model, i);
                related_activated_pos = i;
                update_listview_hw_selected(related_list_view, i);
                break;
            }
        }
    }
    g_free(selected_text);
}

static void append_related_rows(GPtrArray *labels, GPtrArray *payloads) {
    if (!labels || labels->len == 0 || !GTK_IS_STRING_LIST(related_string_list) || !related_row_payloads) {
        return;
    }

    guint start = g_list_model_get_n_items(G_LIST_MODEL(related_string_list));
    char **items = g_new0(char *, labels->len + 1);
    for (guint i = 0; i < labels->len; i++) {
        items[i] = g_ptr_array_index(labels, i);
    }
    gtk_string_list_splice(related_string_list, start, 0, (const char * const *)items);
    g_free(items);

    for (guint i = 0; i < payloads->len; i++) {
        g_ptr_array_add(related_row_payloads, g_ptr_array_index(payloads, i));
    }
    g_ptr_array_set_free_func(payloads, NULL);
    g_ptr_array_set_size(payloads, 0);
}

static gboolean related_payload_matches_word(RelatedRowPayload *payload, const char *word) {
    if (!payload || payload->type != RELATED_ROW_CANDIDATE || !payload->word || !word) {
        return FALSE;
    }

    char *payload_word = normalize_headword_for_search(payload->word, TRUE);
    char *target_word = normalize_headword_for_search(word, TRUE);
    gboolean matches = payload_word && target_word &&
        g_ascii_strcasecmp(payload_word, target_word) == 0;
    g_free(payload_word);
    g_free(target_word);
    return matches;
}

static gboolean select_related_word(const char *word) {
    if (!related_selection_model || !related_row_payloads || !word) {
        return FALSE;
    }

    for (guint i = 0; i < related_row_payloads->len; i++) {
        RelatedRowPayload *payload = g_ptr_array_index(related_row_payloads, i);
        if (related_payload_matches_word(payload, word)) {
            gtk_single_selection_set_selected(related_selection_model, i);
            /* Update hw-selected highlight to follow the matched word */
            related_activated_pos = i;
            update_listview_hw_selected(related_list_view, i);
            return TRUE;
        }
    }

    return FALSE;
}

static void clear_sidebar_list(SidebarListView *sidebar) {
    if (!sidebar) return;
    sidebar->activated_pos = GTK_INVALID_LIST_POSITION;
    if (sidebar->payloads) {
        g_ptr_array_set_size(sidebar->payloads, 0);
    }
    if (GTK_IS_STRING_LIST(sidebar->string_list)) {
        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(sidebar->string_list));
        gtk_string_list_splice(sidebar->string_list, 0, n_items, NULL);
    }
}

static void set_sidebar_list_rows(SidebarListView *sidebar, GPtrArray *labels, GPtrArray *payloads) {
    char *selected_text = NULL;
    if (sidebar && sidebar->selection_model) {
        guint pos = gtk_single_selection_get_selected(sidebar->selection_model);
        if (pos != GTK_INVALID_LIST_POSITION) {
            GtkStringObject *obj = g_list_model_get_item(G_LIST_MODEL(sidebar->string_list), pos);
            if (obj) selected_text = g_strdup(gtk_string_object_get_string(obj));
        }
    }

    clear_sidebar_list(sidebar);
    if (!sidebar || !labels || labels->len == 0 || !GTK_IS_STRING_LIST(sidebar->string_list) || !sidebar->payloads) {
        g_free(selected_text);
        return;
    }

    for (guint i = 0; i < payloads->len; i++) {
        g_ptr_array_add(sidebar->payloads, g_ptr_array_index(payloads, i));
    }
    g_ptr_array_set_free_func(payloads, NULL);
    g_ptr_array_set_size(payloads, 0);

    char **items = g_new0(char *, labels->len + 1);
    for (guint i = 0; i < labels->len; i++) {
        items[i] = g_ptr_array_index(labels, i);
    }
    gtk_string_list_splice(sidebar->string_list, 0, 0, (const char * const *)items);
    g_free(items);

    /* Restore selection if the item is still in the new list */
    if (selected_text && sidebar->selection_model) {
        for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(sidebar->string_list)); i++) {
            GtkStringObject *obj = g_list_model_get_item(G_LIST_MODEL(sidebar->string_list), i);
            if (obj && g_strcmp0(gtk_string_object_get_string(obj), selected_text) == 0) {
                gtk_single_selection_set_selected(sidebar->selection_model, i);
                break;
            }
        }
    }
    g_free(selected_text);
}

static SidebarRowPayload *sidebar_payload_at(SidebarListView *sidebar, guint position) {
    if (!sidebar || !sidebar->payloads || position >= sidebar->payloads->len) {
        return NULL;
    }
    return g_ptr_array_index(sidebar->payloads, position);
}

#if 0
static gboolean sidebar_list_select_payload(SidebarListView *sidebar, SidebarRowPayload *target) {
    if (!sidebar || !sidebar->selection_model || !sidebar->payloads) {
        return FALSE;
    }
    for (guint i = 0; i < sidebar->payloads->len; i++) {
        if (g_ptr_array_index(sidebar->payloads, i) == target) {
            gtk_single_selection_set_selected(sidebar->selection_model, i);
            return TRUE;
        }
    }
    gtk_single_selection_set_selected(sidebar->selection_model, GTK_INVALID_LIST_POSITION);
    return FALSE;
}
#endif

static GtkWidget *sidebar_list_item_make_label(void) {
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_widget_set_margin_start(label, 0);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 4);
    gtk_widget_set_margin_bottom(label, 4);
    return label;
}

static void on_sidebar_favorite_clicked(GtkButton *btn, gpointer user_data);

static void free_signal_data(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static gboolean transform_sidebar_star_visibility(GBinding *binding, const GValue *from_value, GValue *to_value, gpointer user_data) {
    (void)binding; (void)from_value;
    const char *word = user_data;
    gboolean is_favorite = word && word_list_contains_ci(favorite_words, word);
    /* Only show persistently for favorites; hover enter/leave handles the rest */
    g_value_set_boolean(to_value, is_favorite);
    return TRUE;
}

static const char *dict_format_emoji(DictFormat fmt) {
    switch (fmt) {
        case DICT_FORMAT_DSL:      return "📖";
        case DICT_FORMAT_MDX:      return "📘";
        case DICT_FORMAT_BGL:      return "📕";
        case DICT_FORMAT_STARDICT: return "📗";
        case DICT_FORMAT_SLOB:     return "📙";
        default:                   return "📚";
    }
}

static void sidebar_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
static void sidebar_list_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
static void sidebar_list_item_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);

static void on_row_box_enter(GtkEventControllerMotion *ctrl, double x, double y, gpointer ud) {
    (void)x; (void)y; (void)ud;
    GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    GtkWidget *row = gtk_widget_get_parent(box);
    /* Only show hover if not activated */
    if (row && !gtk_widget_has_css_class(row, "hw-selected"))
        gtk_widget_add_css_class(box, "hw-hovered");
    /* Show star button on hover */
    GtkWidget *star_btn = gtk_widget_get_last_child(box);
    if (star_btn && GTK_IS_BUTTON(star_btn))
        gtk_widget_set_visible(star_btn, TRUE);
}

static void on_row_box_leave(GtkEventControllerMotion *ctrl, gpointer ud) {
    (void)ud;
    GtkWidget *box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctrl));
    gtk_widget_remove_css_class(box, "hw-hovered");
    /* Hide star button on leave, unless it's a favorite (starred) */
    GtkWidget *star_btn = gtk_widget_get_last_child(box);
    if (star_btn && GTK_IS_BUTTON(star_btn)) {
        const char *icon_name = gtk_button_get_icon_name(GTK_BUTTON(star_btn));
        gboolean is_fav = icon_name && g_strcmp0(icon_name, "starred-symbolic") == 0;
        if (!is_fav)
            gtk_widget_set_visible(star_btn, FALSE);
    }
}

static void sidebar_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_add_css_class(box, "sidebar-row");
    gtk_widget_set_margin_start(box, 12);
    
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_row_box_enter), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_row_box_leave), NULL);
    gtk_widget_add_controller(box, motion);

    /* File-based icon (shown when dict has an icon image) */
    GtkWidget *icon = gtk_image_new();
    gtk_widget_set_size_request(icon, 16, 16);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    /* Emoji fallback label (shown when no icon image is available) */
    GtkWidget *emoji_lbl = gtk_label_new("");
    gtk_widget_set_valign(emoji_lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(emoji_lbl, FALSE);

    GtkWidget *label = sidebar_list_item_make_label();
    gtk_widget_set_hexpand(label, TRUE);
    
    GtkWidget *star_btn = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_add_css_class(star_btn, "flat");
    gtk_widget_set_valign(star_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(star_btn, 4);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), emoji_lbl);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), star_btn);
    gtk_list_item_set_child(item, box);
}

static void sidebar_list_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    SidebarListView *sidebar = user_data;
    GtkWidget *box      = gtk_list_item_get_child(item);
    GtkWidget *icon     = gtk_widget_get_first_child(box);
    GtkWidget *emoji_lbl = gtk_widget_get_next_sibling(icon);
    GtkWidget *label    = gtk_widget_get_next_sibling(emoji_lbl);
    GtkWidget *star_btn = gtk_widget_get_last_child(box);

    guint position = gtk_list_item_get_position(item);
    GtkWidget *row = gtk_widget_get_parent(box);
    if (row) {
        g_object_set_data(G_OBJECT(row), "row-position", GUINT_TO_POINTER(position));
        if (position == sidebar->activated_pos) {
            gtk_widget_add_css_class(row, "hw-selected");
        } else {
            gtk_widget_remove_css_class(row, "hw-selected");
        }
    }

    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    GtkStringObject *string_object = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    const char *row_text = string_object ? gtk_string_object_get_string(string_object) : "";
    const char *title    = payload && payload->title    ? payload->title    : "";
    const char *subtitle = payload && payload->subtitle ? payload->subtitle : "";
    char *safe_title    = safe_markup_escape_n(row_text, -1);
    char *safe_subtitle = safe_markup_escape_n(subtitle, -1);
    char *markup = NULL;

    /* Helper: show file icon or emoji fallback for dict rows */
    auto void set_dict_icon(void);
    void set_dict_icon(void) {
        if (payload && payload->icon_path && payload->type == SIDEBAR_ROW_DICT) {
            gtk_image_set_from_file(GTK_IMAGE(icon), payload->icon_path);
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
            gtk_widget_set_visible(icon, TRUE);
            gtk_widget_set_visible(emoji_lbl, FALSE);
        } else if (payload && payload->type == SIDEBAR_ROW_DICT && payload->dict_entry) {
            gtk_widget_set_visible(icon, FALSE);
            gtk_label_set_text(GTK_LABEL(emoji_lbl),
                               dict_format_emoji(payload->dict_entry->format));
            gtk_widget_set_visible(emoji_lbl, TRUE);
        } else {
            gtk_widget_set_visible(icon, FALSE);
            gtk_widget_set_visible(emoji_lbl, FALSE);
        }
    }

    if (payload && payload->type == SIDEBAR_ROW_HINT) {
        if (*safe_subtitle) {
            markup = g_strdup_printf("<span alpha='75%%'>%s</span>\n<span alpha='60%%' size='small'>%s</span>",
                                     safe_title, safe_subtitle);
        } else {
            markup = g_strdup_printf("<span alpha='75%%'>%s</span>", safe_title);
        }
        gtk_widget_set_visible(star_btn, FALSE);
        gtk_widget_set_visible(icon, FALSE);
        gtk_widget_set_visible(emoji_lbl, FALSE);
    } else if (*safe_subtitle) {
        markup = g_strdup_printf("%s\n<span alpha='65%%' size='small'>%s</span>",
                                 safe_title, safe_subtitle);
        gtk_widget_set_visible(star_btn, payload->type == SIDEBAR_ROW_WORD);
        set_dict_icon();
    } else {
        markup = g_strdup(safe_title);
        gtk_widget_set_visible(star_btn, payload->type == SIDEBAR_ROW_WORD);
        set_dict_icon();
    }

    gtk_label_set_markup(GTK_LABEL(label), markup);

    g_signal_handlers_disconnect_by_func(star_btn, on_sidebar_favorite_clicked, NULL);
    g_object_set_data(G_OBJECT(star_btn), "bind-item", item);
    
    if (payload && payload->type == SIDEBAR_ROW_WORD) {
        g_signal_connect_data(star_btn, "clicked", G_CALLBACK(on_sidebar_favorite_clicked), g_strdup(title), free_signal_data, 0);
        gboolean is_fav = word_list_contains_ci(favorite_words, title);
        gtk_button_set_icon_name(GTK_BUTTON(star_btn), is_fav ? "starred-symbolic" : "non-starred-symbolic");

        GBinding *binding = g_object_bind_property_full(item, "selected", star_btn, "visible", 
            G_BINDING_SYNC_CREATE,
            transform_sidebar_star_visibility, NULL, g_strdup(title), g_free);
        g_object_set_data(G_OBJECT(item), "star-binding", binding);
    } else {
        gtk_widget_set_visible(star_btn, FALSE);
    }

    g_free(markup);
    g_free(safe_subtitle);
}

static void sidebar_list_item_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory; (void)user_data;
    GBinding *binding = g_object_get_data(G_OBJECT(item), "star-binding");
    if (binding) {
        g_binding_unbind(binding);
        g_object_set_data(G_OBJECT(item), "star-binding", NULL);
    }
    GtkWidget *box = gtk_list_item_get_child(item);
    if (box) {
        GtkWidget *row = gtk_widget_get_parent(box);
        if (row) gtk_widget_remove_css_class(row, "hw-selected");
    }
}

static void populate_history_sidebar(void);
static void populate_favorites_sidebar(void);

static void populate_search_sidebar(const char *query);
static gboolean dict_entry_in_active_scope(DictEntry *entry);

static void cancel_sidebar_search(void) {
    related_activated_pos = GTK_INVALID_LIST_POSITION;
    if (sidebar_search_state) {
        g_atomic_int_set(&sidebar_search_state->cancelled, 1);
        g_clear_pointer(&sidebar_search_state, sidebar_search_state_unref);
    }
}

static void populate_search_sidebar_status(const char *title, const char *subtitle) {
    if (!GTK_IS_STRING_LIST(related_string_list) || !related_row_payloads) {
        return;
    }

    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *payloads = g_ptr_array_new();
    char *label = subtitle && *subtitle
        ? g_strdup_printf("%s\n%s", title ? title : "", subtitle)
        : g_strdup(title ? title : "");
    RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
    payload->type = RELATED_ROW_HINT;
    g_ptr_array_add(labels, label);
    g_ptr_array_add(payloads, payload);
    set_related_rows(labels, payloads);
    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);
}

typedef struct {
    char *label;
    char *sort_key;
    RelatedRowPayload *payload;
    double score;
} BucketItem;

static gint compare_bucket_item(gconstpointer a, gconstpointer b, gpointer user_data) {
    const BucketItem *ia = a;
    const BucketItem *ib = b;
    SearchBucket bucket = GPOINTER_TO_INT(user_data);

    if (bucket == SEARCH_BUCKET_FUZZY) {
        if (ia->score > ib->score) return -1;
        if (ia->score < ib->score) return 1;
    }

    return g_strcmp0(ia->sort_key, ib->sort_key);
}

static gboolean fast_strncasestr(const char *haystack, size_t haystack_len, const char *needle) {
    if (!haystack || !needle) return FALSE;
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return TRUE;

    for (size_t i = 0; i < haystack_len; i++) {
        size_t h_idx = i;
        size_t n_idx = 0;
        
        while (h_idx < haystack_len && n_idx < needle_len) {
            unsigned char hc = (unsigned char)haystack[h_idx];
            
            /* Fast-skip DSL formatting characters during the match, but preserve
             * real spaces so phrase queries like "world bank" still line up. */
            if (hc == '{' || hc == '}' || hc == '\\' || hc == '~' || 
                hc == '/' || hc == ',' || hc == '.' || hc == '-' || 
                hc == '(' || hc == ')' || hc == '[' || hc == ']' || hc == '_' ||
                hc == '*') {
                h_idx++;
                continue;
            }

            if (h_idx + 1 < haystack_len) {
                unsigned char hc1 = (unsigned char)haystack[h_idx + 1];
                if ((hc == 0xC2 && hc1 == 0xB7) ||     /* U+00B7 Middle Dot */
                    (hc == 0xCB && (hc1 == 0x88 || hc1 == 0x8C)) || /* U+02C8, U+02CC */
                    (hc == 0xCC && hc1 == 0x81)) {    /* U+0301 Combining Acute Accent */
                    h_idx += 2;
                    continue;
                }
            }
            
            char nc = needle[n_idx];
            if (g_ascii_tolower(hc) != g_ascii_tolower(nc)) {
                break;
            }
            h_idx++;
            n_idx++;
        }
        if (n_idx == needle_len) {
            return TRUE;
        }
    }
    return FALSE;
}

static GPtrArray *build_search_entry_list(void) {
    GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dict_entry_unref);

    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        dict_entry_ref(entry);
        g_mutex_unlock(&dict_loader_mutex);

        if (entry->dict && entry->dict->index &&
            flat_index_count(entry->dict->index) > 0 &&
            dict_entry_in_active_scope(entry)) {
            g_ptr_array_add(entries, entry);
        } else {
            dict_entry_unref(entry);
        }

        g_mutex_lock(&dict_loader_mutex);
    }
    g_mutex_unlock(&dict_loader_mutex);

    return entries;
}

/* Called from settings-dialog's FTS builder via extern declaration.
 * Returns GPtrArray<DictEntry*> covering ALL loaded dicts (not scope-filtered).
 * Each entry has its ref count incremented; caller must dict_entry_unref each. */
GPtrArray* collect_fts_build_entries(void)
{
    GPtrArray *out = g_ptr_array_new(); /* no auto-free; caller manages refs */
    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (e->dict && e->dict->index &&
            flat_index_count(e->dict->index) > 0) {
            dict_entry_ref(e);
            g_ptr_array_add(out, e);
        }
    }
    g_mutex_unlock(&dict_loader_mutex);
    return out;
}


typedef struct {
    SidebarSearchState *state;
    GPtrArray *labels[BUCKET_COUNT];
    GPtrArray *payloads[BUCKET_COUNT];
    char *status_title;
    char *status_subtitle;
    gboolean is_append;
    gboolean is_finished;
} SearchBatchMsg;

static gboolean sidebar_search_idle_cb(gpointer user_data) {
    SearchBatchMsg *msg = user_data;
    if (msg->state && !g_atomic_int_get(&msg->state->cancelled)) {
        for (int i = 0; i < BUCKET_COUNT; i++) {
            if (msg->labels[i] && msg->labels[i]->len > 0) {
                if (!msg->state->list_started) {
                    set_related_rows(msg->labels[i], msg->payloads[i]);
                    msg->state->list_started = TRUE;
                } else {
                    append_related_rows(msg->labels[i], msg->payloads[i]);
                }
            }
        }
        if (msg->status_title) {
            populate_search_sidebar_status(msg->status_title, msg->status_subtitle);
        }
    }
    for (int i = 0; i < BUCKET_COUNT; i++) {
        if (msg->labels[i]) g_ptr_array_free(msg->labels[i], TRUE);
        if (msg->payloads[i]) g_ptr_array_free(msg->payloads[i], TRUE);
    }
    g_free(msg->status_title);
    g_free(msg->status_subtitle);
    if (msg->state) sidebar_search_state_unref(msg->state);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static gpointer sidebar_search_thread_func(gpointer user_data) {
    SidebarSearchState *state = user_data;
    if (!state) return NULL;

    guint processed = 0;
    const guint max_batch_size = 1000;
    gint64 next_dispatch_time = g_get_monotonic_time() + 50000; // Dispatch every 50ms at most
    guint added = 0;

    // First, do the fast prefix search (formerly seed_search_sidebar_fast_rows)
    if (state->query && state->query_key && !state->is_fts) {
        SearchBatchMsg *seed_msg = g_new0(SearchBatchMsg, 1);
        seed_msg->state = sidebar_search_state_ref(state);
        seed_msg->labels[SEARCH_BUCKET_EXACT] = g_ptr_array_new_with_free_func(g_free);
        seed_msg->payloads[SEARCH_BUCKET_EXACT] = g_ptr_array_new_with_free_func((GDestroyNotify)related_row_payload_free);
        const guint max_seed_rows = 512;
        
        for (guint idx = 0; state->search_entries && idx < state->search_entries->len && added < max_seed_rows; idx++) {
            if (g_atomic_int_get(&state->cancelled)) break;
            DictEntry *entry = g_ptr_array_index(state->search_entries, idx);
            size_t pos = flat_index_search_prefix_fast(entry->dict->index, state->query);
            
            while (pos != (size_t)-1 && added < max_seed_rows) {
                if (g_atomic_int_get(&state->cancelled)) break;
                const FlatTreeEntry *node = flat_index_get(entry->dict->index, pos);
                if (!node) break;

                const char *data_ptr = entry->dict->data ? entry->dict->data : entry->dict->index->mmap_data;
                char *raw_word = g_strndup(data_ptr + node->h_off, node->h_len);
                char *clean_word = normalize_headword_for_search(raw_word, TRUE);

                if (!clean_word || text_has_replacement_char(clean_word)) {
                    g_free(raw_word);
                    if (clean_word) g_free(clean_word);
                    pos++;
                    if (pos >= flat_index_count(entry->dict->index)) break;
                    continue;
                }

                char *word_key = g_utf8_casefold(clean_word, -1);
                SearchBucket bucket;
                double score;

                if (classify_search_candidate_flexible(state->query_key, state->query_len,
                                                       state->query_compact_key, state->query_compact_len,
                                                       word_key, &bucket, &score)) {
                    if (bucket == SEARCH_BUCKET_EXACT || bucket == SEARCH_BUCKET_PREFIX) {
                        char *render_word = normalize_headword_for_render(raw_word, node->h_len, FALSE);
                        GPtrArray *raw_variants = split_headword_variants(raw_word);
                        GPtrArray *render_variants = split_headword_variants(render_word ? render_word : raw_word);

                        for (guint variant_idx = 0; variant_idx < raw_variants->len && added < max_seed_rows; variant_idx++) {
                            const char *raw_variant = g_ptr_array_index(raw_variants, variant_idx);
                            const char *display_variant = variant_idx < render_variants->len
                                ? g_ptr_array_index(render_variants, variant_idx)
                                : raw_variant;
                            char *clean_variant = normalize_headword_for_search(raw_variant, TRUE);
                            char *variant_key = g_utf8_casefold(clean_variant ? clean_variant : raw_variant, -1);
                            g_free(clean_variant);

                            if (!g_hash_table_contains(state->seen_words, variant_key)) {
                                RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                                payload->type = RELATED_ROW_CANDIDATE;
                                payload->word = g_strdup(raw_variant);
                                payload->sort_key = g_utf8_casefold(display_variant ? display_variant : raw_variant, -1);
                                payload->fuzzy_score = score;

                                g_hash_table_add(state->seen_words, g_strdup(variant_key));
                                g_ptr_array_add(seed_msg->labels[SEARCH_BUCKET_EXACT], g_strdup(display_variant));
                                g_ptr_array_add(seed_msg->payloads[SEARCH_BUCKET_EXACT], payload);
                                added++;
                            }
                            g_free(variant_key);
                        }

                        g_ptr_array_free(raw_variants, TRUE);
                        g_ptr_array_free(render_variants, TRUE);
                        g_free(render_word);
                    } else {
                        g_free(raw_word);
                        g_free(word_key);
                        if (clean_word) g_free(clean_word);
                        break;
                    }
                }

                g_free(raw_word);
                if (clean_word) g_free(clean_word);
                g_free(word_key);
                pos++;
                if (pos >= flat_index_count(entry->dict->index)) break;
            }
        }
        
        if (seed_msg->labels[SEARCH_BUCKET_EXACT]->len > 0) {
            g_idle_add(sidebar_search_idle_cb, seed_msg);
        } else {
            for (int i = 0; i < BUCKET_COUNT; i++) {
                if (seed_msg->labels[i]) g_ptr_array_free(seed_msg->labels[i], TRUE);
                if (seed_msg->payloads[i]) g_ptr_array_free(seed_msg->payloads[i], TRUE);
            }
            sidebar_search_state_unref(seed_msg->state);
            g_free(seed_msg);
        }
    }

    if (state->prefix_only) {
        if (added == 0 && !g_atomic_int_get(&state->cancelled)) {
            SearchBatchMsg *final_msg = g_new0(SearchBatchMsg, 1);
            final_msg->state = sidebar_search_state_ref(state);
            final_msg->is_finished = TRUE;
            final_msg->status_title = g_strdup("No results");
            g_idle_add(sidebar_search_idle_cb, final_msg);
        }
        sidebar_search_state_unref(state);
        return NULL;
    }

    if (g_atomic_int_get(&state->cancelled)) {
        sidebar_search_state_unref(state);
        return NULL;
    }

    // Inform UI if searching (only if we haven't already seeded results in standard search)
    if (state->is_fts || added == 0) {
        SearchBatchMsg *status_msg = g_new0(SearchBatchMsg, 1);
        status_msg->state = sidebar_search_state_ref(state);
        if (state->is_fts) {
            if (state->fts_limited) {
                status_msg->status_subtitle = g_strdup_printf("Searching the first %u of %u dictionaries in this scope.",
                                                 state->searched_dict_count,
                                                 state->scoped_dict_count);
                status_msg->status_title = g_strdup("Full Text Search…");
            } else {
                status_msg->status_title = g_strdup("Full Text Search…");
            }
        } else {
            status_msg->status_title = g_strdup("Searching…");
        }
        g_idle_add(sidebar_search_idle_cb, status_msg);
    }

    while (!g_atomic_int_get(&state->cancelled)) {
        if ((!state->search_entries || state->current_entry_index >= state->search_entries->len) &&
            !state->has_current_pos) {
            // END OF SEARCH - DO GLOBAL SORT & FLUSH
            SearchBatchMsg *final_msg = g_new0(SearchBatchMsg, 1);
            final_msg->state = sidebar_search_state_ref(state);
            final_msg->is_finished = TRUE;
            final_msg->is_append = TRUE;

            for (int i = 0; i < BUCKET_COUNT; i++) {
                guint n = state->global_bucket_labels[i]->len;
                if (n > 1) {
                    BucketItem *items = g_new(BucketItem, n);
                    for (guint j = 0; j < n; j++) {
                        RelatedRowPayload *row_payload = g_ptr_array_index(state->global_bucket_payloads[i], j);
                        items[j].label = g_ptr_array_index(state->global_bucket_labels[i], j);
                        items[j].sort_key = row_payload ? row_payload->sort_key : NULL;
                        items[j].payload = row_payload;
                        items[j].score = items[j].payload ? items[j].payload->fuzzy_score : 0.0;
                    }
                    g_sort_array(items, n, sizeof(BucketItem), compare_bucket_item, GINT_TO_POINTER(i));
                    for (guint j = 0; j < n; j++) {
                        g_ptr_array_index(state->global_bucket_labels[i], j) = items[j].label;
                        g_ptr_array_index(state->global_bucket_payloads[i], j) = items[j].payload;
                    }
                    g_free(items);
                }

                if (n > 0) {
                    final_msg->labels[i] = g_ptr_array_new_with_free_func(g_free);
                    final_msg->payloads[i] = g_ptr_array_new_with_free_func((GDestroyNotify)related_row_payload_free);
                    for (guint j = 0; j < n; j++) {
                        g_ptr_array_add(final_msg->labels[i], g_ptr_array_index(state->global_bucket_labels[i], j));
                        g_ptr_array_add(final_msg->payloads[i], g_ptr_array_index(state->global_bucket_payloads[i], j));
                    }
                    g_ptr_array_set_size(state->global_bucket_labels[i], 0);
                    g_ptr_array_set_size(state->global_bucket_payloads[i], 0);
                }
            }

            if (!state->list_started && g_hash_table_size(state->seen_words) == 0) {
                final_msg->status_title = g_strdup("No results");
                if (state->fts_limited) {
                    final_msg->status_subtitle = g_strdup_printf("Searched %u of %u dictionaries in this scope.",
                                                     state->searched_dict_count, state->scoped_dict_count);
                }
            }
            g_idle_add(sidebar_search_idle_cb, final_msg);
            break;
        }

        if (!state->has_current_pos) {
            gboolean found_dict = FALSE;
            while (state->search_entries && state->current_entry_index < state->search_entries->len) {
                DictEntry *entry = g_ptr_array_index(state->search_entries, state->current_entry_index++);
                state->current_pos = 0;
                state->has_current_pos = TRUE;
                state->current_dict_count = flat_index_count(entry->dict->index);
                if (state->current_dict) dict_entry_unref(state->current_dict);
                state->current_dict = entry;
                dict_entry_ref(state->current_dict);
                found_dict = TRUE;
                break;
            }
            if (!found_dict) continue;
        }

        if (!state->has_current_pos || !state->current_dict) {
            state->has_current_pos = FALSE;
            continue;
        }

        const FlatTreeEntry *node = NULL;
        if (state->is_fts) {
            if (!state->fts_regex) {
                state->has_current_pos = FALSE;
                continue;
            }
            size_t match_pos = dict_search_fts(state->current_dict->dict, state->current_dict->path,
                                               state->query, state->fts_regex, state->current_pos,
                                               app_settings && app_settings->fts_enabled);
            if (match_pos == (size_t)-1) {
                state->has_current_pos = FALSE;
                continue;
            }
            state->current_pos = match_pos;
            node = flat_index_get(state->current_dict->dict->index, state->current_pos);
            state->current_pos++;
            if (state->current_pos >= state->current_dict_count) state->has_current_pos = FALSE;
        } else {
            node = flat_index_get(state->current_dict->dict->index, state->current_pos);
            if (!node) {
                state->has_current_pos = FALSE;
                continue;
            }
            state->current_pos++;
            if (state->current_pos >= state->current_dict_count) state->has_current_pos = FALSE;

            const char *data_ptr = state->current_dict->dict->data ? state->current_dict->dict->data : state->current_dict->dict->index->mmap_data;
            if (!state->skip_fast_prefilter &&
                !fast_strncasestr(data_ptr + node->h_off, node->h_len, state->query)) {
                continue;
            }
        }

        const char *data_ptr = state->current_dict->dict->data ? state->current_dict->dict->data : state->current_dict->dict->index->mmap_data;
        char *word = g_strndup(data_ptr + node->h_off, node->h_len);
        char *clean_word = normalize_headword_for_search(word, TRUE);
        if (!clean_word || text_has_replacement_char(clean_word)) {
            g_free(word);
            g_free(clean_word);
            continue;
        }

        char *word_key = g_utf8_casefold(clean_word, -1);
        SearchBucket bucket;
        double fuzzy_score = 0.0;
        gboolean is_valid_match = FALSE;

        if (state->is_fts) {
            is_valid_match = TRUE;
            bucket = SEARCH_BUCKET_SUBSTRING;
            fuzzy_score = 1.0;
        } else {
            is_valid_match = classify_search_candidate_flexible(state->query_key, state->query_len,
                                                                state->query_compact_key, state->query_compact_len,
                                                                word_key, &bucket, &fuzzy_score);
        }

        if (is_valid_match) {
            char *render_word = normalize_headword_for_render(word, node->h_len, FALSE);
            GPtrArray *raw_variants = split_headword_variants(word);
            GPtrArray *render_variants = split_headword_variants(render_word ? render_word : word);

            for (guint variant_idx = 0; variant_idx < raw_variants->len; variant_idx++) {
                const char *raw_variant = g_ptr_array_index(raw_variants, variant_idx);
                const char *display_variant = variant_idx < render_variants->len
                    ? g_ptr_array_index(render_variants, variant_idx)
                    : raw_variant;
                char *clean_variant = normalize_headword_for_search(raw_variant, TRUE);
                char *variant_key = g_utf8_casefold(clean_variant ? clean_variant : raw_variant, -1);
                g_free(clean_variant);

                if (!g_hash_table_contains(state->seen_words, variant_key)) {
                    RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                    payload->type = RELATED_ROW_CANDIDATE;
                    payload->word = g_strdup(raw_variant);
                    payload->sort_key = g_utf8_casefold(display_variant ? display_variant : raw_variant, -1);
                    payload->fuzzy_score = fuzzy_score;

                    g_hash_table_add(state->seen_words, g_strdup(variant_key));
                    
                    int b = (int)bucket;
                    if (b >= 0 && b < BUCKET_COUNT) {
                        g_ptr_array_add(state->global_bucket_labels[b], g_strdup(display_variant));
                        g_ptr_array_add(state->global_bucket_payloads[b], payload);
                        processed++;
                    } else {
                        related_row_payload_free(payload);
                    }
                }
                g_free(variant_key);
            }
            g_ptr_array_free(raw_variants, TRUE);
            g_ptr_array_free(render_variants, TRUE);
            g_free(render_word);
        }
        
        g_free(word);
        if (clean_word) g_free(clean_word);
        g_free(word_key);

        if (processed >= max_batch_size || g_get_monotonic_time() >= next_dispatch_time) {
            if (processed > 0) {
                SearchBatchMsg *batch_msg = g_new0(SearchBatchMsg, 1);
                batch_msg->state = sidebar_search_state_ref(state);
                batch_msg->is_append = TRUE;
                for (int i = 0; i < BUCKET_COUNT; i++) {
                    guint n = state->global_bucket_labels[i]->len;
                    if (n > 0) {
                        batch_msg->labels[i] = g_ptr_array_new_with_free_func(g_free);
                        batch_msg->payloads[i] = g_ptr_array_new_with_free_func((GDestroyNotify)related_row_payload_free);
                        for (guint j = 0; j < n; j++) {
                            g_ptr_array_add(batch_msg->labels[i], g_ptr_array_index(state->global_bucket_labels[i], j));
                            g_ptr_array_add(batch_msg->payloads[i], g_ptr_array_index(state->global_bucket_payloads[i], j));
                        }
                        g_ptr_array_set_size(state->global_bucket_labels[i], 0);
                        g_ptr_array_set_size(state->global_bucket_payloads[i], 0);
                    }
                }
                g_idle_add(sidebar_search_idle_cb, batch_msg);
                processed = 0;
            }
            next_dispatch_time = g_get_monotonic_time() + 50000;
        }
    }

    sidebar_search_state_unref(state);
    return NULL;
}

static void populate_search_sidebar_with_mode(const char *query, gboolean force_fts) {
    cancel_sidebar_search();

    char *clean = normalize_headword_for_search(query, FALSE);
    if (!clean) {
        if (force_fts) {
            populate_search_sidebar_status(
                "Full Text Search",
                "Type a word or phrase to search definitions in this scope.");
        } else {
            populate_search_sidebar_status(
                "Type to search dictionaries…",
                NULL);
        }
        return;
    }

    sidebar_search_state = g_new0(SidebarSearchState, 1);
    sidebar_search_state->ref_count = 1;

    sidebar_search_state->is_fts = force_fts || (clean && g_str_has_prefix(clean, "* "));

    /* Block FTS if the persistent index setting is disabled */
    if (sidebar_search_state->is_fts && !(app_settings && app_settings->fts_enabled)) {
        g_free(clean);
        g_free(sidebar_search_state);
        sidebar_search_state = NULL;
        populate_search_sidebar_status(
            "Full Text Search Unavailable",
            "Enable it in Preferences → System → Search.");
        return;
    }

    char *clean_query = clean;
    if (sidebar_search_state->is_fts) {
        if (clean && g_str_has_prefix(clean, "* ")) {
            clean_query = g_strdup(clean + 2);
            g_free(clean);
            clean = clean_query;
        }
    }

    sidebar_search_state->query = clean ? clean : g_strdup("");
    sidebar_search_state->query_key = g_utf8_casefold(sidebar_search_state->query, -1);
    sidebar_search_state->query_len = utf8_length_or_bytes(sidebar_search_state->query_key);
    sidebar_search_state->query_compact_key = collapse_search_separators(sidebar_search_state->query_key);
    sidebar_search_state->query_compact_len = utf8_length_or_bytes(sidebar_search_state->query_compact_key);
    sidebar_search_state->skip_fast_prefilter = search_query_needs_literal_prefilter_bypass(sidebar_search_state->query);
    sidebar_search_state->prefix_only = !sidebar_search_state->is_fts &&
        sidebar_search_state->query_len <= 3;
    
    if (sidebar_search_state->is_fts && strlen(sidebar_search_state->query) > 0) {
        GError *err = NULL;
        char *escaped = g_regex_escape_string(sidebar_search_state->query, -1);
        sidebar_search_state->fts_regex = g_regex_new(escaped, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, &err);
        g_free(escaped);
        if (err) {
            g_clear_error(&err);
        }
    }

    sidebar_search_state->seen_words = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    sidebar_search_state->search_entries = build_search_entry_list();
    sidebar_search_state->scoped_dict_count = sidebar_search_state->search_entries
        ? sidebar_search_state->search_entries->len
        : 0;
    if (sidebar_search_state->is_fts &&
        sidebar_search_state->search_entries &&
        sidebar_search_state->search_entries->len > MAX_FTS_DICTIONARIES) {
        sidebar_search_state->fts_limited = TRUE;
        g_ptr_array_set_size(sidebar_search_state->search_entries, MAX_FTS_DICTIONARIES);
    }
    sidebar_search_state->searched_dict_count = sidebar_search_state->search_entries
        ? sidebar_search_state->search_entries->len
        : 0;
    
    /* Update FTS highlight query based on current search */
    g_free(fts_highlight_query);
    if (sidebar_search_state->is_fts) {
        fts_highlight_query = g_strdup(sidebar_search_state->query);
    } else {
        fts_highlight_query = NULL;
    }

    for (int i = 0; i < BUCKET_COUNT; i++) {
        sidebar_search_state->global_bucket_labels[i] = g_ptr_array_new();
        sidebar_search_state->global_bucket_payloads[i] = g_ptr_array_new();
    }

    GThread *search_thread = g_thread_try_new("sidebar_search", sidebar_search_thread_func, sidebar_search_state_ref(sidebar_search_state), NULL);
    if (search_thread) {
        g_thread_unref(search_thread);
    } else {
        sidebar_search_state_unref(sidebar_search_state);
        g_clear_pointer(&sidebar_search_state, sidebar_search_state_unref);
    }
}

static void populate_search_sidebar(const char *query) {
    populate_search_sidebar_with_mode(query, FALSE);
}

static void update_favorites_word(const char *word, gboolean add) {
    char *clean = sanitize_user_word(word);
    if (!clean) {
        return;
    }

    if (!favorite_words) {
        favorite_words = g_ptr_array_new_with_free_func(g_free);
    }

    gint existing_idx = word_list_find_ci(favorite_words, clean);
    if (existing_idx >= 0) {
        if (!add) {
            g_ptr_array_remove_index(favorite_words, (guint)existing_idx);
        }
        save_word_list(favorite_words, FAVORITES_FILE_NAME);
        populate_favorites_sidebar();
        populate_history_sidebar();
    
        g_free(clean);
        return;
    }

    if (add) {
        g_ptr_array_insert(favorite_words, 0, clean);
        save_word_list(favorite_words, FAVORITES_FILE_NAME);
        populate_favorites_sidebar();
        populate_history_sidebar();
    
        return;
    }

    g_free(clean);

}

static void update_history_word(const char *word) {
    char *clean = sanitize_user_word(word);
    if (!clean) {
        return;
    }

    if (!history_words) {
        history_words = g_ptr_array_new_with_free_func(g_free);
    }

    gint existing_idx = word_list_find_ci(history_words, clean);
    if (existing_idx >= 0) {
        g_ptr_array_remove_index(history_words, (guint)existing_idx);
    }

    g_ptr_array_insert(history_words, 0, clean);
    while (history_words->len > 200) {
        g_ptr_array_remove_index(history_words, history_words->len - 1);
    }

    save_word_list(history_words, HISTORY_FILE_NAME);
    populate_history_sidebar();
}

static gboolean dict_entry_enabled(DictEntry *entry) {
    if (!entry || !app_settings) {
        return TRUE;
    }
    if (!entry->path) {
        // g_warning("dict_entry_enabled: entry->path is NULL for entry %p!", entry);
        return TRUE;
    }
    return settings_dictionary_enabled_by_path(app_settings, entry->path, TRUE);
}

static gboolean dict_entry_in_scope(DictEntry *entry, const char *scope_id) {
    if (!entry || !dict_entry_enabled(entry)) {
        return FALSE;
    }
    if (scope_id && g_strcmp0(entry->dict_id, scope_id) == 0) {
        return TRUE;
    }
    if (!scope_id || g_strcmp0(scope_id, "all") == 0 || !app_settings) {
        return TRUE;
    }

    gboolean allowed = FALSE;
    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        if (g_strcmp0(grp->id, scope_id) != 0) {
            continue;
        }
        for (guint j = 0; j < grp->members->len; j++) {
            const char *member = g_ptr_array_index(grp->members, j);
            if (g_strcmp0(member, entry->dict_id) == 0) {
                allowed = TRUE;
                break;
            }
        }
        break;
    }
    return allowed;
}

static gboolean dict_entry_in_active_scope(DictEntry *entry) {
    return dict_entry_in_scope(entry, active_scope_id);
}

static gboolean scope_id_exists(const char *scope_id) {
    if (!scope_id || g_strcmp0(scope_id, "all") == 0) {
        return TRUE;
    }

    if (app_settings) {
        for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
            DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
            if (g_strcmp0(grp->id, scope_id) == 0) {
                return TRUE;
            }
        }
    }

    gboolean found = FALSE;
    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (g_strcmp0(e->dict_id, scope_id) == 0) {
            found = TRUE;
            break;
        }
    }
    g_mutex_unlock(&dict_loader_mutex);
    return found;
}

static void ensure_valid_active_scope(void) {
    if (scope_id_exists(active_scope_id)) {
        return;
    }

    g_free(active_scope_id);
    active_scope_id = g_strdup("all");
}

static char *scope_display_name_dup(const char *scope_id) {
    if (!scope_id || g_strcmp0(scope_id, "all") == 0) {
        return g_strdup("All");
    }

    if (app_settings) {
        for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
            DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
            if (g_strcmp0(grp->id, scope_id) == 0) {
                return g_strdup(grp->name ? grp->name : "Group");
            }
        }
    }

    char *found_name = NULL;
    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (g_strcmp0(e->dict_id, scope_id) == 0) {
            found_name = g_strdup(e->name ? e->name : "Dictionary");
            break;
        }
    }
    g_mutex_unlock(&dict_loader_mutex);

    if (found_name) {
        return found_name;
    }

    return g_strdup("All");
}



static DictEntry *find_first_dict_in_active_scope(void) {
    DictEntry *found = NULL;

    g_mutex_lock(&dict_loader_mutex);
    DictEntry *entry = all_dicts;
    while (entry) {
        dict_entry_ref(entry);
        g_mutex_unlock(&dict_loader_mutex);

        if (dict_entry_in_active_scope(entry)) {
            found = entry;
            break;
        }

        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = entry->next;
        dict_entry_unref(entry);
        entry = next;
    }
    if (!found) {
        g_mutex_unlock(&dict_loader_mutex);
    }

    return found;
}

static void ensure_active_entry_in_scope(void) {
    if (active_entry && dict_entry_in_active_scope(active_entry)) {
        return;
    }

    DictEntry *first = find_first_dict_in_active_scope();
    set_active_entry(first);
    if (first) {
        dict_entry_unref(first);
    }
}

typedef struct {
    const char *path;
} MonitorPathPrefix;

static gboolean path_is_inside_directory(const char *path, const char *dir) {
    if (!path || !dir || !*path || !*dir) {
        return FALSE;
    }

    char *expanded_dir = NULL;
    if (dir[0] == '~') {
        expanded_dir = g_build_filename(g_get_home_dir(), dir + 1, NULL);
    } else {
        expanded_dir = g_strdup(dir);
    }

    gsize dir_len = strlen(expanded_dir);
    if (!g_str_has_prefix(path, expanded_dir)) {
        g_free(expanded_dir);
        return FALSE;
    }

    if (path[dir_len] == '\0') {
        g_free(expanded_dir);
        return TRUE;
    }

    gboolean result = expanded_dir[dir_len - 1] == G_DIR_SEPARATOR || path[dir_len] == G_DIR_SEPARATOR;
    g_free(expanded_dir);
    return result;
}

static char *canonicalize_watch_path(const char *path) {
    if (!path || !*path) {
        return NULL;
    }

    if (path[0] == '~') {
        char *expanded = g_build_filename(g_get_home_dir(), path + 1, NULL);
        char *canonical = g_canonicalize_filename(expanded, NULL);
        g_free(expanded);
        return canonical;
    }

    return g_canonicalize_filename(path, NULL);
}

static char *strip_dict_extensions(const char *path) {
    if (!path) return g_strdup("");
    char *p = g_strdup(path);
    size_t len = strlen(p);
    
    /* Strip compressed/double extensions first */
    if (len > 7 && g_ascii_strcasecmp(p + len - 7, ".dsl.dz") == 0) {
        p[len - 7] = '\0';
    } else if (len > 8 && g_ascii_strcasecmp(p + len - 8, ".dict.dz") == 0) {
        p[len - 8] = '\0';
    } else if (len > 8 && g_ascii_strcasecmp(p + len - 8, ".xdxf.dz") == 0) {
        p[len - 8] = '\0';
    } else if (len > 8 && g_ascii_strcasecmp(p + len - 8, ".idx.gz") == 0) {
        p[len - 8] = '\0';
    }
    
    /* Strip single extensions */
    len = strlen(p);
    if (len > 4) {
        if (g_ascii_strcasecmp(p + len - 4, ".dsl") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".mdx") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".ifo") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".idx") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".bgl") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".xdxf") == 0 ||
            g_ascii_strcasecmp(p + len - 4, ".slob") == 0) {
            p[len - 4] = '\0';
        }
    }
    return p;
}

static gboolean identity_path_ends_with_ci(const char *path, const char *suffix) {
    gsize path_len = path ? strlen(path) : 0;
    gsize suffix_len = suffix ? strlen(suffix) : 0;
    return path && suffix && path_len >= suffix_len &&
        g_ascii_strcasecmp(path + path_len - suffix_len, suffix) == 0;
}

static gboolean paths_can_share_dictionary_identity(const char *path1, const char *path2) {
    if (!path1 || !path2) return FALSE;

    gboolean dsl1 = identity_path_ends_with_ci(path1, ".dsl") || identity_path_ends_with_ci(path1, ".dsl.dz");
    gboolean dsl2 = identity_path_ends_with_ci(path2, ".dsl") || identity_path_ends_with_ci(path2, ".dsl.dz");
    if (dsl1 || dsl2) return dsl1 && dsl2;

    gboolean dictd1 = identity_path_ends_with_ci(path1, ".index") ||
                      identity_path_ends_with_ci(path1, ".dict") ||
                      identity_path_ends_with_ci(path1, ".dict.dz");
    gboolean dictd2 = identity_path_ends_with_ci(path2, ".index") ||
                      identity_path_ends_with_ci(path2, ".dict") ||
                      identity_path_ends_with_ci(path2, ".dict.dz");
    if (dictd1 || dictd2) return dictd1 && dictd2;

    gboolean xdxf1 = identity_path_ends_with_ci(path1, ".xdxf") || identity_path_ends_with_ci(path1, ".xdxf.dz");
    gboolean xdxf2 = identity_path_ends_with_ci(path2, ".xdxf") || identity_path_ends_with_ci(path2, ".xdxf.dz");
    if (xdxf1 || xdxf2) return xdxf1 && xdxf2;

    return FALSE;
}

static gboolean paths_are_equivalent(const char *path1, const char *path2) {
    if (!path1 || !path2) return FALSE;
    if (strcmp(path1, path2) == 0) return TRUE;
    
    char *c1 = canonicalize_watch_path(path1);
    char *c2 = canonicalize_watch_path(path2);
    if (!c1 || !c2) {
        g_free(c1);
        g_free(c2);
        return FALSE;
    }
    
    if (strcmp(c1, c2) == 0) {
        g_free(c1);
        g_free(c2);
        return TRUE;
    }
    
    if (!paths_can_share_dictionary_identity(c1, c2)) {
        g_free(c1);
        g_free(c2);
        return FALSE;
    }

    char *s1 = strip_dict_extensions(c1);
    char *s2 = strip_dict_extensions(c2);
    gboolean eq = (strcmp(s1, s2) == 0);
    g_free(s1);
    g_free(s2);
    g_free(c1);
    g_free(c2);
    return eq;
}


static gboolean hash_table_remove_if_path_has_prefix(gpointer key, gpointer value, gpointer user_data) {
    (void)value;
    MonitorPathPrefix *prefix = user_data;
    return path_is_inside_directory((const char *)key, prefix->path);
}

static void remove_directory_monitor_subtree(const char *path) {
    if (!path || !*path || !dictionary_dir_monitors) {
        return;
    }

    MonitorPathPrefix prefix = { path };
    g_hash_table_foreach_remove(dictionary_dir_monitors,
                                hash_table_remove_if_path_has_prefix,
                                &prefix);
}

static gboolean dictionary_monitor_event_requires_reload(GFileMonitorEvent event_type) {
    switch (event_type) {
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_DELETED:
        case G_FILE_MONITOR_EVENT_MOVED:
        case G_FILE_MONITOR_EVENT_RENAMED:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
        case G_FILE_MONITOR_EVENT_MOVED_OUT:
        case G_FILE_MONITOR_EVENT_UNMOUNTED:
            return TRUE;
        default:
            return FALSE;
    }
}

static gboolean reload_dictionaries_from_settings_idle(gpointer user_data);
static void request_dictionary_directory_rescan(gboolean force_directory_rescan);
static void refresh_dictionary_directory_monitors(void);
static void on_dictionary_dir_changed(GFileMonitor *monitor,
                                      GFile *file,
                                      GFile *other_file,
                                      GFileMonitorEvent event_type,
                                      gpointer user_data);
static void on_dictionary_root_parent_changed(GFileMonitor *monitor,
                                              GFile *file,
                                              GFile *other_file,
                                              GFileMonitorEvent event_type,
                                              gpointer user_data);

static void add_directory_monitor_recursive(const char *root_path,
                                            const char *dir_path,
                                            GHashTable *seen_dirs,
                                            int depth) {
    if (depth > 2) return;
    if (!root_path || !dir_path || !*dir_path) {
        return;
    }

    char *canonical_dir = canonicalize_watch_path(dir_path);
    if (!canonical_dir || !g_file_test(canonical_dir, G_FILE_TEST_IS_DIR)) {
        g_free(canonical_dir);
        return;
    }

    if (seen_dirs && g_hash_table_contains(seen_dirs, canonical_dir)) {
        g_free(canonical_dir);
        return;
    }

    if (seen_dirs) {
        g_hash_table_add(seen_dirs, g_strdup(canonical_dir));
    }

    if (!dictionary_dir_monitors) {
        dictionary_dir_monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    }

    if (!g_hash_table_contains(dictionary_dir_monitors, canonical_dir)) {
        GFile *dir_file = g_file_new_for_path(canonical_dir);
        GError *error = NULL;
        GFileMonitor *monitor = g_file_monitor_directory(dir_file,
                                                         G_FILE_MONITOR_WATCH_MOVES,
                                                         NULL,
                                                         &error);
        if (monitor) {
            g_object_set_data_full(G_OBJECT(monitor), "watch-path", g_strdup(canonical_dir), g_free);
            g_object_set_data_full(G_OBJECT(monitor), "watch-root", g_strdup(root_path), g_free);
            g_signal_connect(monitor, "changed", G_CALLBACK(on_dictionary_dir_changed), NULL);
            g_hash_table_insert(dictionary_dir_monitors, g_strdup(canonical_dir), monitor);
        } else if (error) {
            g_error_free(error);
        }
        g_object_unref(dir_file);
    }

    GDir *dir = g_dir_open(canonical_dir, 0, NULL);
    if (dir) {
        const char *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            char *child = g_build_filename(canonical_dir, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
                add_directory_monitor_recursive(root_path, child, seen_dirs, depth + 1);
            }
            g_free(child);
        }
        g_dir_close(dir);
    }

    g_free(canonical_dir);
}

static void ensure_dictionary_root_parent_monitor(const char *root_path) {
    if (!root_path || !*root_path) {
        return;
    }

    if (!dictionary_root_parent_monitors) {
        dictionary_root_parent_monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    }

    if (g_hash_table_contains(dictionary_root_parent_monitors, root_path)) {
        return;
    }

    char *parent_dir = g_path_get_dirname(root_path);
    GFile *dir_file = g_file_new_for_path(parent_dir);
    GError *error = NULL;
    GFileMonitor *monitor = g_file_monitor_directory(dir_file,
                                                     G_FILE_MONITOR_WATCH_MOVES,
                                                     NULL,
                                                     &error);
    if (monitor) {
        g_object_set_data_full(G_OBJECT(monitor), "watch-root", g_strdup(root_path), g_free);
        g_signal_connect(monitor, "changed", G_CALLBACK(on_dictionary_root_parent_changed), NULL);
        g_hash_table_insert(dictionary_root_parent_monitors, g_strdup(root_path), monitor);
    } else if (error) {
        g_error_free(error);
    }

    g_object_unref(dir_file);
    g_free(parent_dir);
}

static void refresh_dictionary_directory_monitors(void) {
    if (!dictionary_dir_monitors) {
        dictionary_dir_monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    } else {
        g_hash_table_remove_all(dictionary_dir_monitors);
    }

    if (!dictionary_root_parent_monitors) {
        dictionary_root_parent_monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    } else {
        g_hash_table_remove_all(dictionary_root_parent_monitors);
    }

    if (!app_settings || !app_settings->dictionary_dirs) {
        return;
    }

    GHashTable *seen_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < app_settings->dictionary_dirs->len; i++) {
        char *root_path = canonicalize_watch_path(g_ptr_array_index(app_settings->dictionary_dirs, i));
        if (!root_path || !*root_path) {
            g_free(root_path);
            continue;
        }

        ensure_dictionary_root_parent_monitor(root_path);
        add_directory_monitor_recursive(root_path, root_path, seen_dirs, 0);
        g_free(root_path);
    }
    g_hash_table_unref(seen_dirs);
}

static gboolean dictionary_root_event_matches_path(const char *root_path, GFile *file) {
    if (!root_path || !file) {
        return FALSE;
    }

    char *file_path = g_file_get_path(file);
    gboolean matches = file_path && g_strcmp0(file_path, root_path) == 0;
    g_free(file_path);
    return matches;
}

static void on_dictionary_dir_changed(GFileMonitor *monitor,
                                      GFile *file,
                                      GFile *other_file,
                                      GFileMonitorEvent event_type,
                                      gpointer user_data) {
    (void)user_data;

    const char *root_path = g_object_get_data(G_OBJECT(monitor), "watch-root");
    if (!root_path || !dictionary_monitor_event_requires_reload(event_type)) {
        return;
    }

    char *file_path = file ? g_file_get_path(file) : NULL;
    if (file_path && (event_type == G_FILE_MONITOR_EVENT_CREATED ||
                      event_type == G_FILE_MONITOR_EVENT_MOVED_IN)) {
        if (g_file_test(file_path, G_FILE_TEST_IS_DIR)) {
            GHashTable *seen_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            add_directory_monitor_recursive(root_path, file_path, seen_dirs, 0);
            g_hash_table_unref(seen_dirs);
        }
    }

    if (file_path && (event_type == G_FILE_MONITOR_EVENT_DELETED ||
                      event_type == G_FILE_MONITOR_EVENT_MOVED_OUT ||
                      event_type == G_FILE_MONITOR_EVENT_UNMOUNTED)) {
        remove_directory_monitor_subtree(file_path);
    }

    if (other_file && (event_type == G_FILE_MONITOR_EVENT_RENAMED ||
                       event_type == G_FILE_MONITOR_EVENT_MOVED)) {
        char *other_path = g_file_get_path(other_file);
        if (other_path && g_file_test(other_path, G_FILE_TEST_IS_DIR)) {
            GHashTable *seen_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            add_directory_monitor_recursive(root_path, other_path, seen_dirs, 0);
            g_hash_table_unref(seen_dirs);
        }
        g_free(other_path);
    }

    g_free(file_path);
    if (dictionary_loading_in_progress) return;
    if (g_get_monotonic_time() < rescan_suppress_until) return;
    request_dictionary_directory_rescan(TRUE);
}

static void on_dictionary_root_parent_changed(GFileMonitor *monitor,
                                              GFile *file,
                                              GFile *other_file,
                                              GFileMonitorEvent event_type,
                                              gpointer user_data) {
    (void)user_data;

    const char *root_path = g_object_get_data(G_OBJECT(monitor), "watch-root");
    if (!root_path || !dictionary_monitor_event_requires_reload(event_type)) {
        return;
    }

    gboolean matches_root = dictionary_root_event_matches_path(root_path, file) ||
                            dictionary_root_event_matches_path(root_path, other_file);
    if (!matches_root) {
        return;
    }

    if (g_file_test(root_path, G_FILE_TEST_IS_DIR)) {
        GHashTable *seen_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        add_directory_monitor_recursive(root_path, root_path, seen_dirs, 0);
        g_hash_table_unref(seen_dirs);
    } else {
        remove_directory_monitor_subtree(root_path);
    }

    if (dictionary_loading_in_progress) return;
    if (g_get_monotonic_time() < rescan_suppress_until) return;
    request_dictionary_directory_rescan(TRUE);
}

static void set_active_entry(DictEntry *new_entry) {
    if (active_entry == new_entry) return;
    DictEntry *old = active_entry;
    active_entry = new_entry;
    if (active_entry) dict_entry_ref(active_entry);
    if (old) dict_entry_unref(old);
}

static DictEntry *dict_entry_new_shell(const char *name, const char *path) {
    if (!path || !*path) {
        return NULL;
    }

    DictEntry *entry = g_new0(DictEntry, 1);
    entry->format = dict_detect_format(path);
    entry->path = g_strdup(path);
    entry->dict_id = settings_make_dictionary_id(path);
    entry->name = g_strdup((name && *name) ? name : path);
    entry->ref_count = 1; entry->magic = 0xDEADC0DE;
    return entry;
}

static DictEntry *find_dict_entry_by_path(const char *path) {
    if (!path) {
        return NULL;
    }

    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        if (g_strcmp0(entry->path, path) == 0) {
            DictEntry *ret = entry;
            dict_entry_ref(ret);
            g_mutex_unlock(&dict_loader_mutex);
            return ret;
        }
    }
    g_mutex_unlock(&dict_loader_mutex);
    return NULL;
}

static guint rebuild_dict_entries_from_settings(void) {
    g_mutex_lock(&dict_loader_mutex);
    DictEntry *old_head = all_dicts;
    DictEntry *new_head = NULL;
    DictEntry *new_tail = NULL;
    guint count = 0;
    char *active_path = active_entry && active_entry->path ? g_strdup(active_entry->path) : NULL;

    GPtrArray *old_entries = g_ptr_array_new();
    GHashTable *existing_by_path = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *reused_entries = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (DictEntry *entry = old_head; entry; entry = entry->next) {
        if (entry->path && !g_hash_table_contains(existing_by_path, entry->path)) {
            g_hash_table_insert(existing_by_path, entry->path, entry);
        }
        g_ptr_array_add(old_entries, entry);
    }

    if (app_settings) {
        for (guint i = 0; i < app_settings->dictionaries->len; i++) {
            if (count >= 1000) break;
            DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, i);

            if (!cfg || !cfg->path || !*cfg->path) {
                continue;
            }

            DictEntry *entry = NULL;
            for (guint j = 0; j < old_entries->len; j++) {
                DictEntry *curr = g_ptr_array_index(old_entries, j);
                if (curr->path && paths_are_equivalent(curr->path, cfg->path)) {
                    entry = curr;
                    break;
                }
            }
            if (!entry) {
                entry = dict_entry_new_shell(cfg->name, cfg->path);
            } else {
                g_hash_table_add(reused_entries, entry);
                g_free(entry->name);
                entry->name = g_strdup((cfg->name && *cfg->name) ? cfg->name : cfg->path);
                if (g_strcmp0(entry->path, cfg->path) != 0) {
                    g_free(entry->path);
                    entry->path = g_strdup(cfg->path);
                    g_free(entry->dict_id);
                    entry->dict_id = settings_make_dictionary_id(cfg->path);
                }
                entry->format = dict_detect_format(cfg->path);
                entry->has_matches = FALSE;
            }

            if (!entry) {
                continue;
            }

            entry->next = NULL;
            if (!new_head) {
                new_head = entry;
            } else {
                new_tail->next = entry;
            }
            new_tail = entry;
            count++;
        }
    }

    for (guint i = 0; i < old_entries->len; i++) {
        DictEntry *entry = g_ptr_array_index(old_entries, i);
        if (!g_hash_table_contains(reused_entries, entry)) {
            entry->next = NULL;
            dict_entry_unref(entry);
        }
    }

    g_ptr_array_free(old_entries, TRUE);

    g_hash_table_unref(existing_by_path);
    g_hash_table_unref(reused_entries);

    all_dicts = new_head;
    g_mutex_unlock(&dict_loader_mutex);
    
    DictEntry *found = active_path ? find_dict_entry_by_path(active_path) : NULL;
    set_active_entry(found ? found : all_dicts);
    if (found) dict_entry_unref(found);
    
    g_free(active_path);
    return count;
}

static gboolean should_rescan_dictionary_dirs(void) {
    if (!app_settings || app_settings->dictionary_dirs->len == 0) {
        return FALSE;
    }

    for (guint i = 0; i < app_settings->dictionary_dirs->len; i++) {
        const char *dir = g_ptr_array_index(app_settings->dictionary_dirs, i);
        char *canonical_dir = canonicalize_watch_path(dir);
        gboolean indexed = FALSE;

        if (!canonical_dir || !g_file_test(canonical_dir, G_FILE_TEST_IS_DIR)) {
            g_free(canonical_dir);
            return TRUE;
        }
        g_free(canonical_dir);

        for (guint j = 0; j < app_settings->dictionaries->len; j++) {
            DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, j);
            if (!cfg || !cfg->path || !*cfg->path) {
                continue;
            }
            if (g_strcmp0(cfg->source, "directory") == 0 &&
                path_is_inside_directory(cfg->path, dir)) {
                indexed = TRUE;
                if (!g_file_test(cfg->path, G_FILE_TEST_EXISTS)) {
                    return TRUE;
                }
                break;
            }
        }

        if (!indexed) {
            for (guint j = 0; j < app_settings->ignored_dictionary_paths->len; j++) {
                const char *ignored = g_ptr_array_index(app_settings->ignored_dictionary_paths, j);
                if (path_is_inside_directory(ignored, dir)) {
                    indexed = TRUE;
                    if (!g_file_test(ignored, G_FILE_TEST_EXISTS)) {
                        return TRUE;
                    }
                    break;
                }
            }
        }

        if (!indexed) {
            return TRUE;
        }
    }

    return FALSE;
}

static void sync_settings_dictionaries_from_loaded(void) {
    if (!app_settings) {
        return;
    }

    GHashTable *loaded_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_mutex_lock(&dict_loader_mutex);
    DictEntry *entry = all_dicts;
    while (entry) {
        dict_entry_ref(entry);
        g_mutex_unlock(&dict_loader_mutex);

        if (entry->dict && entry->path) {
            settings_upsert_dictionary(app_settings, entry->name, entry->path, "directory");
            g_hash_table_add(loaded_paths, g_strdup(entry->path));
        }

        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = entry->next;
        dict_entry_unref(entry);
        entry = next;
    }
    g_mutex_unlock(&dict_loader_mutex);

    settings_prune_directory_dictionaries(app_settings, loaded_paths);
    g_hash_table_unref(loaded_paths);
    settings_save(app_settings);
}



static void populate_history_sidebar(void) {
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *payloads = g_ptr_array_new();

    if (!history_words || history_words->len == 0) {
        SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
        payload->type = SIDEBAR_ROW_HINT;
        payload->title = g_strdup("No history yet");
        payload->subtitle = g_strdup("Successful searches will appear here.");
        g_ptr_array_add(labels, g_strdup(payload->title));
        g_ptr_array_add(payloads, payload);
        set_sidebar_list_rows(&history_sidebar, labels, payloads);
        g_ptr_array_free(labels, TRUE);
        g_ptr_array_free(payloads, TRUE);
        return;
    }

    for (guint i = 0; i < history_words->len; i++) {
        const char *word = g_ptr_array_index(history_words, i);
        GPtrArray *variants = split_headword_variants(word);
        for (guint j = 0; j < variants->len; j++) {
            const char *variant = g_ptr_array_index(variants, j);
            SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
            payload->type = SIDEBAR_ROW_WORD;
            payload->title = g_strdup(variant);
            g_ptr_array_add(labels, g_strdup(variant));
            g_ptr_array_add(payloads, payload);
        }
        g_ptr_array_free(variants, TRUE);
    }

    set_sidebar_list_rows(&history_sidebar, labels, payloads);
    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);
}

static void populate_favorites_sidebar(void) {
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *payloads = g_ptr_array_new();

    if (!favorite_words || favorite_words->len == 0) {
        SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
        payload->type = SIDEBAR_ROW_HINT;
        payload->title = g_strdup("No favorites yet");
        payload->subtitle = g_strdup("Use the star button to save words.");
        g_ptr_array_add(labels, g_strdup(payload->title));
        g_ptr_array_add(payloads, payload);
        set_sidebar_list_rows(&favorites_sidebar, labels, payloads);
        g_ptr_array_free(labels, TRUE);
        g_ptr_array_free(payloads, TRUE);
        return;
    }

    for (guint i = 0; i < favorite_words->len; i++) {
        const char *word = g_ptr_array_index(favorite_words, i);
        GPtrArray *variants = split_headword_variants(word);
        for (guint j = 0; j < variants->len; j++) {
            const char *variant = g_ptr_array_index(variants, j);
            SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
            payload->type = SIDEBAR_ROW_WORD;
            payload->title = g_strdup(variant);
            g_ptr_array_add(labels, g_strdup(variant));
            g_ptr_array_add(payloads, payload);
        }
        g_ptr_array_free(variants, TRUE);
    }

    set_sidebar_list_rows(&favorites_sidebar, labels, payloads);
    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);
}

static void update_search_scope_button_label(void) {
    if (!search_scope_button_label) {
        return;
    }

    ensure_valid_active_scope();
    char *label = scope_display_name_dup(active_scope_id);
    gtk_label_set_text(search_scope_button_label, label ? label : "All");
    gtk_widget_set_tooltip_text(GTK_WIDGET(search_scope_button), label ? label : "All");
    g_free(label);
}

static void rebuild_search_scope_menu(void) {
    if (!search_scope_button) {
        return;
    }

    ensure_valid_active_scope();

    GMenu *menu = g_menu_new();

    GMenuItem *all_item = g_menu_item_new("All", NULL);
    g_menu_item_set_action_and_target(all_item, "app.search-scope", "s", "all");
    g_menu_append_item(menu, all_item);
    g_object_unref(all_item);

    if (app_settings && app_settings->dictionary_groups->len > 0) {
        GMenu *group_submenu = g_menu_new();
        for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
            DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
            GMenuItem *item = g_menu_item_new(grp->name ? grp->name : "Group", NULL);
            g_menu_item_set_action_and_target(item, "app.search-scope", "s", grp->id);
            g_menu_append_item(group_submenu, item);
            g_object_unref(item);
        }
        g_menu_append_submenu(menu, "Groups", G_MENU_MODEL(group_submenu));
        g_object_unref(group_submenu);
    }

    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (!dict_entry_enabled(e)) continue;
        GMenuItem *item = g_menu_item_new(e->name ? e->name : "Dictionary", NULL);
        g_menu_item_set_action_and_target(item, "app.search-scope", "s", e->dict_id);
        g_menu_append_item(menu, item);
        g_object_unref(item);
    }
    g_mutex_unlock(&dict_loader_mutex);

    gtk_menu_button_set_menu_model(search_scope_button, G_MENU_MODEL(menu));
    g_object_unref(menu);

    if (search_scope_action) {
        g_simple_action_set_state(search_scope_action, g_variant_new_string(active_scope_id ? active_scope_id : "all"));
    }
}

static void set_active_scope(const char *scope_id, gboolean refresh_results) {
    const char *desired_scope = (scope_id && scope_id_exists(scope_id)) ? scope_id : "all";

    if (g_strcmp0(active_scope_id, desired_scope) == 0) {
        update_search_scope_button_label();
        if (search_scope_action) {
            g_simple_action_set_state(search_scope_action, g_variant_new_string(desired_scope));
        }
        if (!refresh_results) {
            return;
        }
    } else {
        g_free(active_scope_id);
        active_scope_id = g_strdup(desired_scope);
        ensure_active_entry_in_scope();
        update_search_scope_button_label();
        if (search_scope_action) {
            g_simple_action_set_state(search_scope_action, g_variant_new_string(desired_scope));
        }
    }

    if (refresh_results) {
        refresh_search_results();
    } else {

        populate_dict_sidebar();
    }
}



static gboolean dict_entry_visible_in_sidebar(DictEntry *entry) {
    if (!entry || !dict_entry_enabled(entry)) {
        return FALSE;
    }
    if (!search_entry) {
        return TRUE;
    }
    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (!query || strlen(query) == 0) {
        return TRUE;
    }
    return entry->has_matches;
}

static gboolean is_media_url(const char *uri) {
    if (!uri) return FALSE;
    char *lower = g_ascii_strdown(uri, -1);
    char *qmark = strchr(lower, '?');
    if (qmark) *qmark = '\0';
    gboolean is_media = g_str_has_suffix(lower, ".mp3") ||
                        g_str_has_suffix(lower, ".wav") ||
                        g_str_has_suffix(lower, ".ogg") ||
                        g_str_has_suffix(lower, ".oga") ||
                        g_str_has_suffix(lower, ".spx") ||
                        g_str_has_suffix(lower, ".flac") ||
                        g_str_has_suffix(lower, ".m4a") ||
                        g_str_has_suffix(lower, ".aac") ||
                        g_str_has_suffix(lower, ".wma");
    g_free(lower);
    return is_media;
}

static char *resolve_audio_resource_from_dictionaries(const char *resource_dir,
                                                      const char *sound_file,
                                                      gpointer user_data) {
    (void)user_data;
    char *audio_path = NULL;

    g_mutex_lock(&dict_loader_mutex);
    DictEntry *e = all_dicts;
    while (e) {
        dict_entry_ref(e);
        g_mutex_unlock(&dict_loader_mutex);

        if (e->dict && e->dict->resource_dir && g_strcmp0(e->dict->resource_dir, resource_dir) == 0) {
            if (e->dict->resource_reader) {
                fprintf(stderr, "[AUDIO DEBUG] Searching ResourceReader for '%s'\n", sound_file);
                audio_path = resource_reader_get(e->dict->resource_reader, sound_file);
                if (audio_path) {
                    dict_entry_unref(e);
                    break;
                }
            }
        }

        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = e->next;
        dict_entry_unref(e);
        e = next;
    }
    g_mutex_unlock(&dict_loader_mutex);

    return audio_path;
}

static void on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType t, gpointer user_data) {
    (void)v;
    /* target="_new"/"_blank" links fire NEW_WINDOW_ACTION — open them externally */
    if (t == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(d);
        WebKitNavigationAction *na = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = webkit_navigation_action_get_request(na);
        const char *uri = webkit_uri_request_get_uri(req);
        if (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
            fprintf(stderr, "[EXTERNAL NEW WINDOW]: '%s'\n", uri);
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
        }
        webkit_policy_decision_ignore(d);
        return;
    }
    if (t == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(d);
        WebKitNavigationAction *na = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = webkit_navigation_action_get_request(na);
        const char *uri = webkit_uri_request_get_uri(req);
        if (g_str_has_prefix(uri, "dict://")) {
            char *unescaped = g_uri_unescape_string(uri + 7, NULL);
            fprintf(stderr, "[LINK CLICKED]: '%s'\n", unescaped ? unescaped : uri + 7);
            g_free(unescaped);
        } else if (g_str_has_prefix(uri, "entry://") || g_str_has_prefix(uri, "bword://")) {
            char *unescaped = g_uri_unescape_string(uri + 8, NULL);
            fprintf(stderr, "[LINK CLICKED]: '%s'\n", unescaped ? unescaped : uri + 8);
            g_free(unescaped);
        } else if (g_str_has_prefix(uri, "sound://")) {
            /* Keep existing audio logic or omit logging if not requested */
        } else if (is_media_url(uri) && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
            fprintf(stderr, "[MEDIA URL CLICKED]: '%s'\n", uri);
        } else if (g_strcmp0(uri, "file:///") != 0) {
            fprintf(stderr, "[LINK CLICKED]: '%s'\n", uri);
        }
        if (g_str_has_prefix(uri, "dict://")) {
            const char *word = uri + 7;
            char *unescaped = g_uri_unescape_string(word, NULL);
            char *clean_link = normalize_headword_for_search(unescaped ? unescaped : word, TRUE);
            const char *final_word = clean_link ? clean_link : (unescaped ? unescaped : word);
            gtk_editable_set_text(GTK_EDITABLE(user_data), final_word);
            execute_search_now_for_query(final_word, TRUE);
            g_free(clean_link);
            g_free(unescaped);
            webkit_policy_decision_ignore(d);
            return;
        } else if (g_str_has_prefix(uri, "entry://") || g_str_has_prefix(uri, "bword://")) {
            const char *word = uri + 8;
            char *unescaped = g_uri_unescape_string(word, NULL);
            char *clean_link = normalize_headword_for_search(unescaped ? unescaped : word, TRUE);
            const char *final_word = clean_link ? clean_link : (unescaped ? unescaped : word);
            gtk_editable_set_text(GTK_EDITABLE(user_data), final_word);
            execute_search_now_for_query(final_word, TRUE);
            g_free(clean_link);
            g_free(unescaped);
            webkit_policy_decision_ignore(d);
            return;
        } else if (g_str_has_prefix(uri, "gdlookup://localhost/")) {
            const char *word = uri + 21;
            char *unescaped = g_uri_unescape_string(word, NULL);
            char *clean_link = normalize_headword_for_search(unescaped ? unescaped : word, TRUE);
            const char *final_word = clean_link ? clean_link : (unescaped ? unescaped : word);
            gtk_editable_set_text(GTK_EDITABLE(user_data), final_word);
            execute_search_now_for_query(final_word, TRUE);
            g_free(clean_link);
            g_free(unescaped);
            webkit_policy_decision_ignore(d);
            return;
        } else if (g_str_has_prefix(uri, "sound://")) {
            if (!audio_try_play_encoded_sound_uri(uri, resolve_audio_resource_from_dictionaries, NULL)) {
                const char *sound_file = uri + 8; // Skip "sound://"
                fprintf(stderr, "[AUDIO CLICKED] Clicked: %s\n", sound_file);
                
                /* Backward-compatible fallback and lazy loading */
                if (active_entry && active_entry->dict) {
                    char *audio_path = NULL;
                    if (active_entry->dict->resource_reader) {
                        audio_path = resource_reader_get(active_entry->dict->resource_reader, sound_file);
                    }
                    if (!audio_path && active_entry->dict->resource_dir) {
                        audio_path = g_build_filename(active_entry->dict->resource_dir, sound_file, NULL);
                    }
                    if (!audio_path && active_entry->dict->source_dir) {
                        audio_path = g_build_filename(active_entry->dict->source_dir, sound_file, NULL);
                    }

                    if (audio_path && g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
                        audio_play_file(audio_path);
                    } else {
                        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path ? audio_path : sound_file);
                    }
                    g_free(audio_path);
                } else {
                    fprintf(stderr, "[AUDIO ERROR] No active dictionary entry\n");
                }
            }
            
            webkit_policy_decision_ignore(d);
            return;
        } else if (is_media_url(uri) && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
            audio_play_file(uri);
            webkit_policy_decision_ignore(d);
            return;
        } else if (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")) {
            /* Open external web links in the default browser, not inside the dict WebView */
            fprintf(stderr, "[EXTERNAL URL]: '%s'\n", uri);
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
            webkit_policy_decision_ignore(d);
            return;
        }
    }
    webkit_policy_decision_use(d);
}

static void related_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
static void related_list_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
static void on_related_item_activated(GtkListView *view, guint position, gpointer user_data);
static void on_history_item_activated(GtkListView *view, guint position, gpointer user_data);
static void on_favorites_item_activated(GtkListView *view, guint position, gpointer user_data);

static void on_dict_item_activated(GtkListView *view, guint position, gpointer user_data);

static void append_rendered_word_html_impl(const char *raw_word, gboolean push_history);
static void append_rendered_word_html(const char *raw_word);



static GtkWidget *create_sidebar_list_view(SidebarListView *sidebar, GCallback activate_cb) {
    sidebar->activated_pos = GTK_INVALID_LIST_POSITION;
    sidebar->string_list = gtk_string_list_new(NULL);
    sidebar->payloads = g_ptr_array_new_with_free_func((GDestroyNotify)sidebar_row_payload_free);
    sidebar->selection_model = GTK_SINGLE_SELECTION(gtk_single_selection_new(G_LIST_MODEL(sidebar->string_list)));
    gtk_single_selection_set_autoselect(sidebar->selection_model, FALSE);
    gtk_single_selection_set_can_unselect(sidebar->selection_model, FALSE);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(sidebar_list_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(sidebar_list_item_bind), sidebar);
    g_signal_connect(factory, "unbind", G_CALLBACK(sidebar_list_item_unbind), sidebar);

    sidebar->list_view = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sidebar->selection_model), factory));
    gtk_list_view_set_single_click_activate(sidebar->list_view, TRUE);
    g_signal_connect(sidebar->list_view, "activate", activate_cb, sidebar);
    return GTK_WIDGET(sidebar->list_view);
}

/*
 * Directly toggle hw-selected CSS class on visible rows of a GtkListView.
 * This avoids gtk_string_list_splice which fires items-changed and causes
 * the list view to scroll back to the top.
 */
static void update_listview_hw_selected(GtkListView *list_view, guint activated_pos) {
    if (!list_view) return;
    /* GtkListView children are the row widgets (GtkListItemWidget).
     * Each row's first child is the box set via gtk_list_item_set_child(). */
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list_view));
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        /* The row widget exposes a "position" property via GtkListItem.
         * We can get the position by inspecting the child box's inner label
         * or by checking the GtkListItem accessible role.
         * Simpler: GtkListItemWidget has the child widget whose parent is `child` (the row).
         * The GtkListItem is accessible via the listitem CSS node.
         * Actually, just walk children: the row widget itself has a child (the box).
         * We need to use gtk_list_item_get_position, but we don't have the GtkListItem.
         * GTK4 doesn't expose a public API from row widget → GtkListItem.
         *
         * Alternative approach: use "position" data we stash on the row during bind.
         */
        guint pos = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "row-position"));
        /* pos 0 is ambiguous (could be position 0 or not-set). Use a sentinel. */
        gboolean has_pos = g_object_get_data(G_OBJECT(child), "row-position") != NULL
                           || pos == 0;
        if (!has_pos) continue;

        if (pos == activated_pos) {
            gtk_widget_add_css_class(child, "hw-selected");
        } else {
            gtk_widget_remove_css_class(child, "hw-selected");
        }
    }
}

static void on_history_item_activated(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    SidebarListView *sidebar = user_data;
    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    if (payload && payload->type == SIDEBAR_ROW_WORD && payload->title) {
        append_rendered_word_html(payload->title);
    }
    if (sidebar && sidebar->selection_model) {
        gtk_single_selection_set_selected(sidebar->selection_model, position);
    }
    
    sidebar->activated_pos = position;
    update_listview_hw_selected(sidebar->list_view, position);
}

static void on_find_next(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (web_view)
        webkit_find_controller_search_next(webkit_web_view_get_find_controller(web_view));
}

static void on_find_prev(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (web_view)
        webkit_find_controller_search_previous(webkit_web_view_get_find_controller(web_view));
}

static void on_find_close(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (find_revealer) {
        gtk_revealer_set_reveal_child(find_revealer, FALSE);
        if (web_view) {
            webkit_find_controller_search_finish(webkit_web_view_get_find_controller(web_view));
            const char *clear_js = 
                "(function() {"
                "  const marks = document.querySelectorAll('mark.diction-match');"
                "  marks.forEach(m => {"
                "    const parent = m.parentNode;"
                "    if (!parent) return;"
                "    while(m.firstChild) parent.insertBefore(m.firstChild, m);"
                "    parent.removeChild(m);"
                "  });"
                "})();";
            webkit_web_view_evaluate_javascript(web_view, clear_js, -1, NULL, NULL, NULL, NULL, NULL);
        }
        gtk_widget_grab_focus(GTK_WIDGET(web_view));
    }
}

static void on_find_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)user_data;
    if (!web_view) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    WebKitFindController *fc = webkit_web_view_get_find_controller(web_view);
    if (!text || strlen(text) == 0) {
        webkit_find_controller_search_finish(fc);
        if (find_status_label) gtk_label_set_text(find_status_label, "");
        return;
    }
    webkit_find_controller_search(fc, text, 
        WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, 
        G_MAXUINT);

    /* Manual highlighting to ensure readability in dark mode */
    char *escaped_text = g_strescape(text, NULL);
    char *js = g_strdup_printf(
        "(function(text) {"
        "  function clear() {"
        "    const marks = document.querySelectorAll('mark.diction-match');"
        "    marks.forEach(m => {"
        "      const parent = m.parentNode;"
        "      if (!parent) return;"
        "      while(m.firstChild) parent.insertBefore(m.firstChild, m);"
        "      parent.removeChild(m);"
        "    });"
        "  }"
        "  clear();"
        "  if (!text) return;"
        "  const regex = new RegExp(text.replace(/[-\\/\\\\^$*+?.()|[\\]{}]/g, '\\\\$&'), 'gi');"
        "  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null, false);"
        "  const nodes = [];"
        "  let node;"
        "  while(node = walker.nextNode()) nodes.push(node);"
        "  nodes.forEach(node => {"
        "    const p = node.parentNode;"
        "    if (p && (p.tagName === 'SCRIPT' || p.tagName === 'STYLE' || p.tagName === 'MARK')) return;"
        "    const val = node.nodeValue;"
        "    let match;"
        "    const matches = [];"
        "    while ((match = regex.exec(val)) !== null) matches.push(match);"
        "    for (let i = matches.length - 1; i >= 0; i--) {"
        "      const m = matches[i];"
        "      const range = document.createRange();"
        "      try {"
        "        range.setStart(node, m.index);"
        "        range.setEnd(node, m.index + m[0].length);"
        "        const mark = document.createElement('mark');"
        "        mark.className = 'diction-match';"
        "        range.surroundContents(mark);"
        "      } catch(e) {}"
        "    }"
        "  });"
        "})('%s');", escaped_text);
    
    webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
    g_free(escaped_text);
}

static void on_find_counted_matches(WebKitFindController *fc, guint count, gpointer user_data) {
    (void)fc; (void)user_data;
    if (find_status_label) {
        if (count == 0) {
            gtk_label_set_text(find_status_label, "No matches");
        } else {
            char buf[32];
            g_snprintf(buf, sizeof(buf), "%u matches", count);
            gtk_label_set_text(find_status_label, buf);
        }
    }
}

static gboolean on_find_shortcut_close(GtkWidget *widget, GVariant *args, gpointer user_data) {
    (void)widget; (void)args; (void)user_data;
    on_find_close(NULL, NULL);
    return TRUE;
}

static void on_find_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    if (find_revealer) {
        gboolean active = gtk_revealer_get_reveal_child(find_revealer);
        gtk_revealer_set_reveal_child(find_revealer, !active);
        if (!active && find_bar_entry) {
            gtk_widget_grab_focus(GTK_WIDGET(find_bar_entry));
            const char *text = gtk_editable_get_text(GTK_EDITABLE(find_bar_entry));
            if (text && strlen(text) > 0) {
                on_find_search_changed(find_bar_entry, NULL);
            }
        } else {
            gtk_widget_grab_focus(GTK_WIDGET(web_view));
        }
    }
}

static GtkWidget* create_find_bar() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    find_bar_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(find_bar_entry), TRUE);
    g_object_set(find_bar_entry, "placeholder-text", "Find in page...", NULL);
    g_signal_connect(find_bar_entry, "search-changed", G_CALLBACK(on_find_search_changed), NULL);
    g_signal_connect(find_bar_entry, "activate", G_CALLBACK(on_find_next), NULL);

    find_status_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(find_status_label), "dim-label");
    gtk_widget_set_margin_end(GTK_WIDGET(find_status_label), 6);

    GtkWidget *prev_btn = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_add_css_class(prev_btn, "flat");
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_find_prev), NULL);

    GtkWidget *next_btn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_add_css_class(next_btn, "flat");
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_find_next), NULL);

    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_find_close), NULL);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(find_bar_entry));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(find_status_label));
    gtk_box_append(GTK_BOX(box), prev_btn);
    gtk_box_append(GTK_BOX(box), next_btn);
    gtk_box_append(GTK_BOX(box), close_btn);

    find_revealer = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(find_revealer, GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_child(find_revealer, box);

    return GTK_WIDGET(find_revealer);
}

static void related_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
static void related_list_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
static void related_list_item_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);

static void related_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_add_css_class(box, "sidebar-row");
    gtk_widget_set_margin_start(box, 12);

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_row_box_enter), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_row_box_leave), NULL);
    gtk_widget_add_controller(box, motion);
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_margin_start(label, 0);
    gtk_widget_set_margin_top(label, 4);
    gtk_widget_set_margin_bottom(label, 4);
    
    GtkWidget *star_btn = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_add_css_class(star_btn, "flat");
    gtk_widget_set_valign(star_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(star_btn, 4);

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), star_btn);
    gtk_list_item_set_child(item, box);
}

static void on_sidebar_favorite_clicked(GtkButton *btn, gpointer user_data) {
    char *word = g_strdup(user_data);
    if (!word) return;
    gboolean is_favorite_now = word_list_contains_ci(favorite_words, word);
    update_favorites_word(word, !is_favorite_now);
    gboolean is_favorited = !is_favorite_now;
    gtk_button_set_icon_name(btn, is_favorited ? "starred-symbolic" : "non-starred-symbolic");
    
    GtkListItem *item = g_object_get_data(G_OBJECT(btn), "bind-item");
    if (item) {
        gboolean selected = gtk_list_item_get_selected(item);
        gtk_widget_set_visible(GTK_WIDGET(btn), selected || is_favorited);
    }
    g_free(word);
}

static void related_list_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_list_item_get_child(item);
    GtkWidget *label = gtk_widget_get_first_child(box);
    GtkWidget *star_btn = gtk_widget_get_last_child(box);
    
    GtkStringObject *string_object = GTK_STRING_OBJECT(gtk_list_item_get_item(item));
    guint position = gtk_list_item_get_position(item);
    GtkWidget *row = gtk_widget_get_parent(box);
    if (row) {
        g_object_set_data(G_OBJECT(row), "row-position", GUINT_TO_POINTER(position));
        if (position == related_activated_pos) {
            gtk_widget_add_css_class(row, "hw-selected");
        } else {
            gtk_widget_remove_css_class(row, "hw-selected");
        }
    }

    const char *text = string_object ? gtk_string_object_get_string(string_object) : "";
    char *valid_text = g_utf8_make_valid(text ? text : "", -1);
    RelatedRowPayload *payload = NULL;

    if (related_row_payloads && position < related_row_payloads->len) {
        payload = g_ptr_array_index(related_row_payloads, position);
    }

    if (payload && payload->type == RELATED_ROW_HINT) {
        char *escaped = g_markup_escape_text(valid_text, -1);
        char *markup = g_strdup_printf("<span alpha='75%%'>%s</span>", escaped);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        gtk_widget_set_visible(star_btn, FALSE);
        g_free(markup);
        g_free(escaped);
        g_free(valid_text);
        return;
    }

    gtk_label_set_text(GTK_LABEL(label), valid_text);
    
    g_signal_handlers_disconnect_by_func(star_btn, on_sidebar_favorite_clicked, NULL);
    const char *favorite_word = (payload && payload->word) ? payload->word : valid_text;
    g_signal_connect_data(star_btn, "clicked", G_CALLBACK(on_sidebar_favorite_clicked), g_strdup(favorite_word), free_signal_data, 0);
    
    gboolean is_fav = word_list_contains_ci(favorite_words, favorite_word);
    gtk_button_set_icon_name(GTK_BUTTON(star_btn), is_fav ? "starred-symbolic" : "non-starred-symbolic");

    GBinding *binding = g_object_bind_property_full(item, "selected", star_btn, "visible", 
        G_BINDING_SYNC_CREATE,
        transform_sidebar_star_visibility, NULL, g_strdup(favorite_word), g_free);
    g_object_set_data(G_OBJECT(item), "star-binding", binding);

    g_free(valid_text);
}

static void related_list_item_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory; (void)user_data;
    GBinding *binding = g_object_get_data(G_OBJECT(item), "star-binding");
    if (binding) {
        g_binding_unbind(binding);
        g_object_set_data(G_OBJECT(item), "star-binding", NULL);
    }
    GtkWidget *box = gtk_list_item_get_child(item);
    if (box) {
        GtkWidget *row = gtk_widget_get_parent(box);
        if (row) gtk_widget_remove_css_class(row, "hw-selected");
    }
}

static void on_related_item_activated(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    (void)user_data;
    if (!related_row_payloads || position >= related_row_payloads->len) {
        return;
    }

    RelatedRowPayload *payload = g_ptr_array_index(related_row_payloads, position);
    if (!payload || payload->type != RELATED_ROW_CANDIDATE || !payload->word) {
        return;
    }

    fprintf(stderr, "[Result Clicked]: '%s'\n", payload->word);
    if (related_selection_model) {
        gtk_single_selection_set_selected(related_selection_model, position);
    }
    
    related_activated_pos = position;
    update_listview_hw_selected(related_list_view, position);

    append_rendered_word_html(payload->word);
}

static void on_favorites_item_activated(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    SidebarListView *sidebar = user_data;
    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    if (payload && payload->type == SIDEBAR_ROW_WORD && payload->title) {
        append_rendered_word_html(payload->title);
    }
    if (sidebar && sidebar->selection_model) {
        gtk_single_selection_set_selected(sidebar->selection_model, position);
    }

    sidebar->activated_pos = position;
    update_listview_hw_selected(sidebar->list_view, position);
}





static void on_dict_item_activated(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    SidebarListView *sidebar = user_data;
    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    if (!payload || payload->type != SIDEBAR_ROW_DICT || !payload->dict_entry) {
        return;
    }
    DictEntry *target_entry = payload->dict_entry;
    activate_dictionary_entry(target_entry);
    populate_dict_sidebar();

    /* Find the new payload corresponding to the activated dictionary */
    for (guint i = 0; i < sidebar->payloads->len; i++) {
        SidebarRowPayload *p = g_ptr_array_index(sidebar->payloads, i);
        if (p->type == SIDEBAR_ROW_DICT && p->dict_entry == target_entry) {
            gtk_single_selection_set_selected(sidebar->selection_model, i);
            
            sidebar->activated_pos = i;
            update_listview_hw_selected(sidebar->list_view, i);
            break;
        }
    }
}

static gboolean run_debounced_search(gpointer user_data) {
    (void)user_data;
    search_execute_source_id = 0;
    execute_search_now();
    return G_SOURCE_REMOVE;
}

static void schedule_execute_search(void) {
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
    }
    search_execute_source_id = g_timeout_add(200, run_debounced_search, NULL);
}



static char* render_entry_def_to_html(DictEntry *entry, const FlatTreeEntry *res) {
    char *to_free = NULL;
    size_t def_len = 0;
    const char *def_ptr = dict_get_definition(entry->dict, res, &def_len, &to_free);

    if (!def_ptr) return NULL;

    if (entry->format == DICT_FORMAT_MDX && def_len > 8 && g_str_has_prefix(def_ptr, "@@@LINK=")) {
        char link_target[1024];
        const char *lp = def_ptr + 8;
        size_t l = 0;
        while (l < sizeof(link_target) - 1 && l < (def_len - 8) && lp[l] != '\r' && lp[l] != '\n' && lp[l] != '\0') {
            link_target[l] = lp[l];
            l++;
        }
        link_target[l] = '\0';

        size_t red_pos = flat_index_search(entry->dict->index, link_target);
        if (red_pos != (size_t)-1) {
            const FlatTreeEntry *red_res = flat_index_get(entry->dict->index, red_pos);
            if (red_res) {
                if (to_free) g_free(to_free);
                to_free = NULL;
                def_ptr = dict_get_definition(entry->dict, red_res, &def_len, &to_free);
            }
        } else {
            /* Redirect target not found — render a clickable link instead of raw text */
            char *escaped_target = g_markup_escape_text(link_target, -1);
            char *uri_target = g_uri_escape_string(link_target, NULL, FALSE);
            char *redirect_html = g_strdup_printf(
                "<p style='margin:0.5em 0;'>&#8594; <a href='dict://%s'>%s</a></p>",
                uri_target, escaped_target);
            g_free(escaped_target);
            g_free(uri_target);
            return redirect_html;
        }
    }

    int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
    const char *render_style = (app_settings && app_settings->render_style && *app_settings->render_style)
        ? app_settings->render_style
        : "diction";

    dict_render_set_resource_reader(entry->dict->resource_reader);
    const char *hw_data_ptr = entry->dict->data ? entry->dict->data : entry->dict->index->mmap_data;
    char *html = dsl_render_body_only(
        def_ptr, def_len,
        hw_data_ptr + res->h_off, res->h_len,
        entry->format, entry->dict->resource_dir, entry->dict->source_dir, entry->dict->mdx_stylesheet, dark_mode,
        app_settings ? app_settings->color_theme : "default",
        render_style,
        fts_highlight_query);
    if (to_free) g_free(to_free);
    return html;
}



static GPtrArray *split_headword_variants(const char *headword) {
    GPtrArray *variants = g_ptr_array_new_with_free_func(g_free);
    if (!headword || !*headword) {
        g_ptr_array_add(variants, g_strdup(""));
        return variants;
    }

    char **parts = g_strsplit(headword, "; ", -1);
    for (guint i = 0; parts && parts[i]; i++) {
        char *copy = g_strdup(parts[i]);
        g_strstrip(copy);
        if (*copy) {
            g_ptr_array_add(variants, copy);
        } else {
            g_free(copy);
        }
    }
    g_strfreev(parts);

    if (variants->len == 0) {
        g_ptr_array_add(variants, g_strdup(headword));
    }

    return variants;
}






static void update_nav_buttons_state(void);

static GPtrArray *get_current_nav_history(void) {
    if (!tab_view) return NULL;
    AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
    if (!page) return NULL;
    GPtrArray *arr = g_object_get_data(G_OBJECT(page), "nav-history");
    if (!arr) {
        arr = g_ptr_array_new_with_free_func(nav_history_item_free);
        g_object_set_data_full(G_OBJECT(page), "nav-history", arr, (GDestroyNotify)g_ptr_array_unref);
    }
    return arr;
}
#define nav_history get_current_nav_history()

static int get_current_nav_index(void) {
    if (!tab_view) return -1;
    AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
    if (!page) return -1;
    gpointer ptr = g_object_get_data(G_OBJECT(page), "nav-history-index");
    if (!ptr && !g_object_get_data(G_OBJECT(page), "nav-history-index-set")) {
        g_object_set_data(G_OBJECT(page), "nav-history-index", GINT_TO_POINTER(-1));
        g_object_set_data(G_OBJECT(page), "nav-history-index-set", GINT_TO_POINTER(1));
        return -1;
    }
    return GPOINTER_TO_INT(ptr);
}
static void set_current_nav_index(int val) {
    if (!tab_view) return;
    AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
    if (!page) return;
    g_object_set_data(G_OBJECT(page), "nav-history-index", GINT_TO_POINTER(val));
    g_object_set_data(G_OBJECT(page), "nav-history-index-set", GINT_TO_POINTER(1));
}
#define nav_history_index get_current_nav_index()

static void update_nav_buttons_state(void) {
    if (nav_back_btn) {
        gtk_widget_set_sensitive(nav_back_btn, nav_history && nav_history_index > 0);
    }
    if (nav_forward_btn) {
        gtk_widget_set_sensitive(nav_forward_btn, nav_history && nav_history_index < (int)nav_history->len - 1);
    }
}

static void push_to_nav_history(const char *view_word, const char *search_query, gboolean search_is_fts) {
    GPtrArray *hist = nav_history;
    int idx = nav_history_index;
    if (!hist) return;

    char *clean_view = sanitize_user_word(view_word);
    char *clean_query = sanitize_user_word(search_query);
    if (!clean_view || !clean_query) {
        g_free(clean_view);
        g_free(clean_query);
        return;
    }
    
    if (idx >= 0 && idx < (int)hist->len) {
        NavHistoryItem *current = g_ptr_array_index(hist, idx);
        if (g_ascii_strcasecmp(current->view_word, clean_view) == 0 &&
            g_ascii_strcasecmp(current->search_query, clean_query) == 0 &&
            current->search_is_fts == search_is_fts) {
            g_free(clean_view);
            g_free(clean_query);
            return;
        }
    }
    
    if (idx >= 0 && idx < (int)hist->len - 1) {
        g_ptr_array_remove_range(hist, idx + 1, hist->len - idx - 1);
    }
    
    if (hist->len > 0) {
        NavHistoryItem *last = g_ptr_array_index(hist, hist->len - 1);
        if (g_ascii_strcasecmp(last->view_word, clean_view) == 0 &&
            g_ascii_strcasecmp(last->search_query, clean_query) == 0 &&
            last->search_is_fts == search_is_fts) {
            set_current_nav_index(hist->len - 1);
            g_free(clean_view);
            g_free(clean_query);
            update_nav_buttons_state();
            return;
        }
    }
    
    NavHistoryItem *item = g_new0(NavHistoryItem, 1);
    item->view_word = clean_view;
    item->search_query = clean_query;
    item->search_is_fts = search_is_fts;
    g_ptr_array_add(hist, item);
    set_current_nav_index(hist->len - 1);
    update_nav_buttons_state();
}

static void execute_search_now_for_query(const char *query_raw, gboolean push_history);

static void navigate_to_history_item(NavHistoryItem *item) {
    AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
    set_tab_full_text_search(page, item->search_is_fts);

    if (g_ascii_strcasecmp(item->view_word, item->search_query) == 0) {
        populate_search_sidebar_with_mode(item->search_query, item->search_is_fts);
        execute_search_now_for_query(item->search_query, FALSE);
    } else {
        append_rendered_word_html_impl(item->view_word, FALSE);
        populate_search_sidebar_with_mode(item->search_query, item->search_is_fts);
    }
}

static void on_nav_back_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    GPtrArray *hist = nav_history;
    int idx = nav_history_index;
    if (hist && idx > 0) {
        idx--;
        set_current_nav_index(idx);
        NavHistoryItem *item = g_ptr_array_index(hist, idx);
        navigate_to_history_item(item);
        update_nav_buttons_state();
    }
}

static void on_nav_forward_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    GPtrArray *hist = nav_history;
    int idx = nav_history_index;
    if (hist && idx < (int)hist->len - 1) {
        idx++;
        set_current_nav_index(idx);
        NavHistoryItem *item = g_ptr_array_index(hist, idx);
        navigate_to_history_item(item);
        update_nav_buttons_state();
    }
}

static void set_tab_metadata(WebKitWebView *wv, const char *query, const char *title, int is_firm) {
    if (!tab_view || !wv) return;
    GtkWidget *scroll = gtk_widget_get_ancestor(GTK_WIDGET(wv), GTK_TYPE_SCROLLED_WINDOW);
    if (!scroll) return;
    AdwTabPage *page = adw_tab_view_get_page(tab_view, scroll);
    if (page) {
        if (query) g_object_set_data_full(G_OBJECT(page), "search-query", g_strdup(query), g_free);
        if (title) adw_tab_page_set_title(page, title);
        g_object_set_data(G_OBJECT(page), "is-firm", GINT_TO_POINTER(is_firm));
    }
}

static gboolean tab_page_is_full_text_search(AdwTabPage *page) {
    return page && GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "search-is-fts"));
}

static void update_search_mode_visuals(gboolean is_fts) {
    const char *icon_name = is_fts ? "search-dictionary-symbolic" : "system-search-symbolic";
    const char *placeholder = is_fts ? "Full Text Search" : "Search";

    /* Update the collapsed-button icon */
    if (search_mode_icon) {
        gtk_image_set_from_icon_name(search_mode_icon, icon_name);
    }

    /* Update the primary icon inside the GtkEntry (works because search_entry
     * is now a plain GtkEntry, not GtkSearchEntry which manages its own icon). */
    if (search_entry) {
        gtk_entry_set_icon_from_icon_name(search_entry, GTK_ENTRY_ICON_PRIMARY, icon_name);
        gtk_entry_set_placeholder_text(search_entry, placeholder);
    }

    /* Update the collapsed button label (only when showing a generic placeholder) */
    if (search_button_label) {
        const char *lbl = gtk_label_get_text(search_button_label);
        if (!lbl || !*lbl ||
            g_strcmp0(lbl, "Search") == 0 ||
            g_strcmp0(lbl, "Full Text Search") == 0) {
            gtk_label_set_text(GTK_LABEL(search_button_label), placeholder);
        }
    }
}

static void sync_full_text_search_action_state(void) {
    gboolean is_fts = tab_view && tab_page_is_full_text_search(adw_tab_view_get_selected_page(tab_view));
    update_search_mode_visuals(is_fts);
}

static void set_tab_full_text_search(AdwTabPage *page, gboolean is_fts) {
    if (!page) {
        return;
    }

    g_object_set_data(G_OBJECT(page), "search-is-fts", GINT_TO_POINTER(is_fts));

    if (tab_view && page == adw_tab_view_get_selected_page(tab_view)) {
        sync_full_text_search_action_state();
    }
}

static gboolean current_tab_is_full_text_search(void) {
    return tab_view && tab_page_is_full_text_search(adw_tab_view_get_selected_page(tab_view));
}

static gboolean query_requests_full_text_search(const char *query_raw, gboolean preferred_fts) {
    gboolean use_fts = preferred_fts;
    char *clean_query = normalize_headword_for_search(query_raw, FALSE);
    if (clean_query && g_str_has_prefix(clean_query, "* ")) {
        use_fts = TRUE;
    }
    g_free(clean_query);
    return use_fts;
}

static char *exact_lookup_definite_article_variant(const char *query) {
    if (!query || !*query) {
        return NULL;
    }

    if (g_ascii_strncasecmp(query, "the ", 4) == 0) {
        return NULL;
    }

    return g_strdup_printf("the %s", query);
}

typedef struct {
    DictEntry *dict;
    const FlatTreeEntry *entry;
    char *raw_hw;
    char *clean_hw;
    char *display_hw;
} ExactMatch;

static int compare_exact_match_items(gconstpointer a, gconstpointer b) {
    const ExactMatch *ma = *(const ExactMatch **)a;
    const ExactMatch *mb = *(const ExactMatch **)b;
    int cmp = g_strcmp0(ma->clean_hw, mb->clean_hw);
    if (cmp != 0) return cmp;
    /* Tie-breaker: sort by dictionary order */
    return 0; 
}

static void exact_match_free(gpointer data) {
    ExactMatch *m = data;
    dict_entry_unref(m->dict);
    g_free(m->raw_hw);
    g_free(m->clean_hw);
    g_free(m->display_hw);
    g_free(m);
}

static int append_exact_matches_html(GString *html_res, const char *query, gboolean *limited) {
    int found_count = 0;
    GPtrArray *matches = g_ptr_array_new_with_free_func(exact_match_free);
    if (limited) *limited = FALSE;

    g_mutex_lock(&dict_loader_mutex);
    DictEntry *e = all_dicts;
    while (e) {
        dict_entry_ref(e);
        g_mutex_unlock(&dict_loader_mutex);

        if (!e->dict || !dict_entry_in_active_scope(e)) {
            g_mutex_lock(&dict_loader_mutex);
            DictEntry *next = e->next;
            dict_entry_unref(e);
            e = next;
            continue;
        }

        size_t qlen = strlen(query);
        /* Use binary-only search — avoids the O(N) alias fallback scan that
         * would otherwise linearly scan ALL entries of this dict. The outer loop
         * already iterates every dictionary, so the fallback is pure waste. */
        size_t pos = flat_index_search_fast(e->dict->index, query);
        while (pos != (size_t)-1) {
            const FlatTreeEntry *res = flat_index_get(e->dict->index, pos);
            if (!res) break;
            const char *data_ptr = e->dict->data ? e->dict->data : e->dict->index->mmap_data;
            if (!flat_index_entry_matches_query(data_ptr, res, query, qlen)) break;

            ExactMatch *m = g_new0(ExactMatch, 1);
            m->dict = e;
            dict_entry_ref(e);
            m->entry = res;
            m->raw_hw = g_strndup(data_ptr + res->h_off, res->h_len);
            m->clean_hw = normalize_headword_for_search(m->raw_hw, TRUE);
            m->display_hw = normalize_headword_for_render(m->raw_hw, strlen(m->raw_hw), FALSE);
            
            g_ptr_array_add(matches, m);
            if (matches->len >= MAX_EXACT_RENDERED_MATCHES) {
                if (limited) *limited = TRUE;
                break;
            }
            pos++;
            if (pos >= flat_index_count(e->dict->index)) break;
        }

        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = e->next;
        dict_entry_unref(e);
        e = next;
        if (limited && *limited) break;
    }
    g_mutex_unlock(&dict_loader_mutex);

    if (matches->len == 0) {
        g_ptr_array_unref(matches);
        return 0;
    }

    g_ptr_array_sort(matches, compare_exact_match_items);

    const char *render_style = (app_settings && app_settings->render_style && *app_settings->render_style)
        ? app_settings->render_style : "diction";

    for (guint i = 0; i < matches->len; i++) {
        ExactMatch *m = g_ptr_array_index(matches, i);
        char *rendered = render_entry_def_to_html(m->dict, m->entry);
        if (rendered) {
            m->dict->has_matches = TRUE;
            char *escaped_hw = safe_markup_escape_n(m->display_hw, -1);
            char *escaped_dn = g_markup_escape_text(m->dict->name, -1);
            const char *emoji = dict_format_emoji(m->dict->format);

            g_string_append_printf(html_res, 
                "<section id='dict-%s' class='%s-entry'>"
                "<div class='%s-header'>"
                "<span class='%s-lemma'>%s</span>"
                "<span class='%s-dict'>%s %s</span>"
                "</div>"
                "<div class='%s-entry-body'>%s</div>"
                "</section>",
                m->dict->dict_id ? m->dict->dict_id : "", render_style, 
                render_style, render_style, escaped_hw,
                render_style, emoji ? emoji : "📖", escaped_dn,
                render_style, rendered);

            g_free(escaped_hw);
            g_free(escaped_dn);
            g_free(rendered);
            found_count++;
        }
    }

    if (limited && *limited) {
        g_string_append_printf(
            html_res,
            "<p style='opacity:.72;font-size:.92em;margin:12px 8px;'>Showing first %d matching dictionaries. Narrow the search or choose a dictionary scope for more.</p>",
            MAX_EXACT_RENDERED_MATCHES);
    }

    g_ptr_array_unref(matches);
    return found_count;
}

static void render_query_to_webview(const char *query_raw, WebKitWebView *target_wv, gboolean push_history) {
    if (!target_wv) return;

    char *query = normalize_headword_for_search(query_raw, FALSE);
    gboolean should_highlight_fts =
        query_requests_full_text_search(query_raw, current_tab_is_full_text_search()) &&
        fts_highlight_query && *fts_highlight_query;

    if (query && g_str_has_prefix(query, "* ")) {
        char *stripped = g_strdup(query + 2);
        g_free(query);
        query = stripped;
    }

    if (!query || strlen(query) == 0) {
        queue_fts_highlight_for_web_view(target_wv, NULL);
        render_idle_page_to_webview(target_wv, "Diction", "Start typing to search...");
        set_tab_metadata(target_wv, "", "Diction", 0);
        g_free(query);
        return;
    }

    int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
    char *shared_css = dict_render_shared_styles(
        dark_mode,
        app_settings ? app_settings->color_theme : "default",
        app_settings ? app_settings->font_family : NULL,
        app_settings ? app_settings->font_size : 0);

    GString *html_res = g_string_new("<html><head>");
    if (shared_css) {
        g_string_append(html_res, shared_css);
        g_free(shared_css);
    }
    g_string_append(html_res, "</head><body>");

    char *escaped_query_attr = safe_markup_escape_n(query, -1);
    g_string_append_printf(html_res, "<div class='word-group' data-word='%s'>", escaped_query_attr);
    g_free(escaped_query_attr);
    gsize html_prefix_len = html_res->len;

    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *e = all_dicts; e; e = e->next) e->has_matches = FALSE;
    g_mutex_unlock(&dict_loader_mutex);

    gboolean exact_limited = FALSE;
    int found_count = append_exact_matches_html(html_res, query, &exact_limited);
    if (found_count == 0) {
        char *fallback_query = exact_lookup_definite_article_variant(query);
        if (fallback_query) {
            g_string_truncate(html_res, html_prefix_len);
            exact_limited = FALSE;
            found_count = append_exact_matches_html(html_res, fallback_query, &exact_limited);
            g_free(fallback_query);
        }
    }

    if (found_count > 0) {
        queue_fts_highlight_for_web_view(target_wv,
                                         should_highlight_fts ? fts_highlight_query : NULL);
        g_string_append(html_res, "</div></body></html>");
        webkit_web_view_load_html(target_wv, html_res->str, "file:///");
        set_tab_metadata(target_wv, query, query, 1);
        /* Auto-highlight the matching word in the search sidebar */
        select_related_word(query);
        if (push_history && target_wv == get_current_web_view()) {
            update_history_word(query);
            const char *current_search_query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
            push_to_nav_history(query, current_search_query, current_tab_is_full_text_search());
        }
    } else {
        queue_fts_highlight_for_web_view(target_wv, NULL);
        set_tab_metadata(target_wv, query, "No Match", 1);
        char *escaped_query = safe_markup_escape_n(query, -1);
        char *message = g_strdup_printf(
            "No exact match for <b>%s</b> in any dictionary.",
            escaped_query ? escaped_query : query);
        render_idle_page_to_webview(target_wv, "No Match", message);
        g_free(message);
        g_free(escaped_query);
    }
    g_string_free(html_res, TRUE);
    g_free(query);

    /* Refresh the Dictionaries sidebar so it shows only dicts with results */
    populate_dict_sidebar();
}

static void execute_search_now_for_query(const char *query_raw, gboolean push_history) {
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }
    render_query_to_webview(query_raw, get_current_web_view(), push_history);
}

static void execute_search_now(void) {
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }

    if (!search_entry) {
        return;
    }

    const char *query_raw = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    execute_search_now_for_query(query_raw, TRUE);
}

static void apply_fts_highlight_to_web_view(WebKitWebView *wv, const char *query) {
    if (!wv || !query || !*query) {
        return;
    }

    char *escaped_text = g_strescape(query, NULL);
    char *js = g_strdup_printf(
        "(function(text) {"
        "  function clear() {"
        "    const marks = document.querySelectorAll('span.fts-highlight');"
        "    marks.forEach(m => {"
        "      const parent = m.parentNode;"
        "      if (!parent) return;"
        "      while (m.firstChild) parent.insertBefore(m.firstChild, m);"
        "      parent.removeChild(m);"
        "    });"
        "  }"
        "  clear();"
        "  if (!text) return;"
        "  const regex = new RegExp(text.replace(/[-\\/\\\\^$*+?.()|[\\]{}]/g, '\\\\$&'), 'gi');"
        "  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null, false);"
        "  const nodes = [];"
        "  let node;"
        "  while ((node = walker.nextNode())) nodes.push(node);"
        "  nodes.forEach(node => {"
        "    const p = node.parentNode;"
        "    if (p && (p.tagName === 'SCRIPT' || p.tagName === 'STYLE' || p.classList?.contains('fts-highlight'))) return;"
        "    const val = node.nodeValue;"
        "    let match;"
        "    const matches = [];"
        "    while ((match = regex.exec(val)) !== null) matches.push(match);"
        "    for (let i = matches.length - 1; i >= 0; i--) {"
        "      const m = matches[i];"
        "      const range = document.createRange();"
        "      try {"
        "        range.setStart(node, m.index);"
        "        range.setEnd(node, m.index + m[0].length);"
        "        const span = document.createElement('span');"
        "        span.className = 'fts-highlight';"
        "        range.surroundContents(span);"
        "      } catch (e) {}"
        "    }"
        "  });"
        "})('%s');",
        escaped_text);

    webkit_web_view_evaluate_javascript(wv, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
    g_free(escaped_text);
}

static void queue_fts_highlight_for_web_view(WebKitWebView *wv, const char *query) {
    if (!wv) {
        return;
    }

    g_object_set_data_full(G_OBJECT(wv),
                           "pending-fts-highlight-query",
                           query && *query ? g_strdup(query) : NULL,
                           g_free);
}

static void append_rendered_word_html_impl(const char *raw_word, gboolean push_history) {
    char *query = normalize_headword_for_search(raw_word, TRUE);
    
    /* Strip FTS prefix for headword matching in the renderer */
    if (query && g_str_has_prefix(query, "* ")) {
        char *stripped = g_strdup(query + 2);
        g_free(query);
        query = stripped;
    }

    if (!query || strlen(query) == 0) {
        g_free(query);
        return;
    }

    select_related_word(query);

    char *display_title = normalize_headword_for_render(raw_word, raw_word ? strlen(raw_word) : 0, FALSE);

    GString *html_res = g_string_new("");
    gboolean exact_limited = FALSE;
    int found_count = append_exact_matches_html(html_res, query, &exact_limited);
    if (found_count == 0) {
        char *fallback_query = exact_lookup_definite_article_variant(query);
        if (fallback_query) {
            g_string_truncate(html_res, 0);
            exact_limited = FALSE;
            found_count = append_exact_matches_html(html_res, fallback_query, &exact_limited);
            g_free(fallback_query);
        }
    }

    if (found_count > 0) {
        const char *current_search_query = search_entry
            ? gtk_editable_get_text(GTK_EDITABLE(search_entry))
            : NULL;
        gboolean should_highlight_fts =
            query_requests_full_text_search(current_search_query, current_tab_is_full_text_search()) &&
            fts_highlight_query && *fts_highlight_query;

        set_tab_metadata(get_current_web_view(), query, display_title, 1);
        
        if (push_history) {
            update_history_word(query);
            push_to_nav_history(query, current_search_query,
                                query_requests_full_text_search(current_search_query, current_tab_is_full_text_search()));
        }

        WebKitWebView *wv = get_current_web_view();
        if (wv) {
            queue_fts_highlight_for_web_view(wv,
                                             should_highlight_fts ? fts_highlight_query : NULL);
                                             
            int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
            char *shared_css = dict_render_shared_styles(
                dark_mode,
                app_settings ? app_settings->color_theme : "default",
                app_settings ? app_settings->font_family : NULL,
                app_settings ? app_settings->font_size : 0);
                
            GString *full_html = g_string_new("<html><head>");
            if (shared_css) {
                g_string_append(full_html, shared_css);
                g_free(shared_css);
            }
            g_string_append(full_html, "</head><body>");
            
            char *escaped_attr = safe_markup_escape_n(query, -1);
            g_string_append_printf(full_html, "<div class='word-group' data-word='%s'>", escaped_attr);
            g_free(escaped_attr);
            
            g_string_append(full_html, html_res->str);
            g_string_append(full_html, "</div></body></html>");

            webkit_web_view_load_html(wv, full_html->str, "file:///");
            g_string_free(full_html, TRUE);
        }
    }
    
    g_free(display_title);
    g_free(query);
    g_string_free(html_res, TRUE);
}

static void append_rendered_word_html(const char *raw_word) {
    append_rendered_word_html_impl(raw_word, TRUE);
}

static void on_search_changed(GtkEditable *entry, gpointer user_data) {
    (void)user_data;
    
    if (gtk_widget_has_focus(GTK_WIDGET(entry))) {
        if (tab_view) {
            AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
            if (page) {
                gboolean is_firm = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "is-firm"));
                if (is_firm) {
                    g_signal_handlers_block_by_func(tab_view, on_tab_selected, NULL);
                    create_new_tab("Search", TRUE);
                    g_signal_handlers_unblock_by_func(tab_view, on_tab_selected, NULL);
                }
            }
        }
    }
    
    // Automatically update the title of the present tab to match what we progressively type
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));
    char *display_query = normalize_headword_for_render(query, query ? strlen(query) : 0, FALSE);

    if (tab_view) {
        AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
        if (page) {
            adw_tab_page_set_title(page, (display_query && *display_query) ? display_query : "Home");
            g_object_set_data_full(G_OBJECT(page), "search-query", g_strdup(query), g_free);
        }
    }

    if (search_button_label) {
        gtk_label_set_text(GTK_LABEL(search_button_label), (display_query && *display_query) ? display_query : "Search");
    }
    
    g_free(display_query);
    update_search_mode_visuals(current_tab_is_full_text_search());

    gboolean is_fts = current_tab_is_full_text_search();
    if (is_fts) {
        if (search_execute_source_id != 0) {
            g_source_remove(search_execute_source_id);
            search_execute_source_id = 0;
        }

        if (!query || strlen(query) == 0) {
            cancel_sidebar_search();
            g_clear_pointer(&fts_highlight_query, g_free);
            populate_search_sidebar_status("Full Text Search", "Type a word or phrase to search definitions in this scope.");
        } else {
            populate_search_sidebar_with_mode(query, TRUE);
        }
    } else {
        populate_search_sidebar_with_mode(query, FALSE);
    }

    if (last_search_query && strcmp(query, last_search_query) == 0) return;

    g_free(last_search_query);
    last_search_query = g_strdup(query);

    if (is_fts) {
        return;
    }

    if (!query || strlen(query) == 0) {
        execute_search_now();
        return;
    }

    schedule_execute_search();
}

static gboolean on_random_word_found_idle(gpointer user_data);

typedef struct {
    char *word;
    char *clean_hw;
} RandomWordIdleData;

static gboolean on_random_word_found_idle(gpointer user_data) {
    RandomWordIdleData *id = user_data;
    if (id->clean_hw) {
        gtk_editable_set_text(GTK_EDITABLE(search_entry), id->clean_hw);
        if (search_button_label) {
            gtk_label_set_text(GTK_LABEL(search_button_label), id->clean_hw);
        }

        execute_search_now_for_query(id->clean_hw, TRUE);
    }
    g_free(id->word);
    g_free(id->clean_hw);
    g_free(id);
    return G_SOURCE_REMOVE;
}

static gpointer random_word_thread_worker(gpointer data) {
    (void)data;
    g_mutex_lock(&dict_loader_mutex);
    int count = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (e->dict && e->dict->index && flat_index_count(e->dict->index) > 0 && dict_entry_in_active_scope(e)) {
            count++;
        }
    }
    
    DictEntry *target_e = NULL;
    if (count > 0) {
        int target_idx = rand() % count;
        DictEntry *curr = all_dicts;
        int cur_count = 0;
        while (curr) {
            if (curr->dict && curr->dict->index && flat_index_count(curr->dict->index) > 0 && dict_entry_in_active_scope(curr)) {
                if (cur_count == target_idx) {
                    target_e = curr;
                    dict_entry_ref(target_e);
                    break;
                }
                cur_count++;
            }
            curr = curr->next;
        }
    }
    g_mutex_unlock(&dict_loader_mutex);

    if (target_e) {
        int attempts = 0;
        const FlatTreeEntry *node = NULL;
        char *found_word = NULL;
        char *clean_hw = NULL;

        while (attempts < 15) {
            node = flat_index_random(target_e->dict->index);
            if (!node) break;

            /* This read might block on disk I/O, which is why we are in a background thread */
            const char *data_ptr = target_e->dict->data ? target_e->dict->data : target_e->dict->index->mmap_data;
            const char *raw_data = data_ptr + node->h_off;
            found_word = g_strndup(raw_data, node->h_len);
            clean_hw = normalize_headword_for_render(found_word, node->h_len, FALSE);

            if (clean_hw && *clean_hw && !text_has_replacement_char(clean_hw)) {
                break;
            }

            g_free(found_word);
            g_free(clean_hw);
            found_word = NULL;
            clean_hw = NULL;
            attempts++;
        }

        if (clean_hw) {
            RandomWordIdleData *id = g_new0(RandomWordIdleData, 1);
            id->word = found_word;
            id->clean_hw = clean_hw;
            g_idle_add(on_random_word_found_idle, id);
        }
        dict_entry_unref(target_e);
    }

    return NULL;
}

static void on_random_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (!all_dicts) return;
    /* Launch background thread to avoid UI freeze during I/O */
    g_thread_unref(g_thread_new("random-word-picker", random_word_thread_worker, NULL));
}

static void maybe_show_startup_random_word(void) {
    return; // DEBUG: Disable startup random word


    const char *current = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (current && *current) {
        startup_random_word_pending = FALSE;
        return;
    }

    int loaded_count = 0;
    g_mutex_lock(&dict_loader_mutex);
    DictEntry *e = all_dicts;
    while (e) {
        dict_entry_ref(e);
        g_mutex_unlock(&dict_loader_mutex);

        if (e->dict && e->dict->index && flat_index_count(e->dict->index) > 0 && dict_entry_in_active_scope(e)) {
            loaded_count++;
            dict_entry_unref(e);
            break;
        }

        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = e->next;
        dict_entry_unref(e);
        e = next;
    }
    if (e == NULL) g_mutex_unlock(&dict_loader_mutex);

    if (loaded_count == 0) {
        return;
    }

    startup_random_word_pending = FALSE;
    on_random_clicked(NULL, NULL);
}

static void activate_dictionary_entry(DictEntry *e) {
    if (!e || !e->dict_id) return;

    char js[256];
    snprintf(js, sizeof(js),
        "var el = document.getElementById('dict-%s'); "
        "if (el) { el.scrollIntoView({behavior: 'smooth', block: 'start'}); }",
        e->dict_id);
    webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);
    set_active_entry(e);
    populate_dict_sidebar();
}

// Refresh the current search results when theme changes
// Refresh the current search results when theme changes
static void refresh_search_results(void) {
    if (!tab_view) return;
    rescan_suppress_until = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

    GListModel *pages = G_LIST_MODEL(adw_tab_view_get_pages(tab_view));
    guint n_pages = g_list_model_get_n_items(pages);
    AdwTabPage *selected_page = adw_tab_view_get_selected_page(tab_view);
    for (guint i = 0; i < n_pages; i++) {
        AdwTabPage *page = ADW_TAB_PAGE(g_list_model_get_item(pages, i));
        GtkWidget *scroll = adw_tab_page_get_child(page);
        WebKitWebView *wv = get_web_view_from_scroll(scroll);
        if (wv) {
            const char *query = (const char *)g_object_get_data(G_OBJECT(page), "search-query");
            const char *live_query = search_entry
                ? gtk_editable_get_text(GTK_EDITABLE(search_entry))
                : NULL;
            if ((!query || !*query) && page == selected_page && live_query && *live_query) {
                query = live_query;
            }

            if (query && *query) {
                render_query_to_webview(query, wv, FALSE);
            } else {
                render_idle_page_to_webview(wv, "Diction", "Start typing to search...");
            }
        }
    }
    
    // Also refresh the sidebars based on current selection
    const char *main_query = search_entry ? gtk_editable_get_text(GTK_EDITABLE(search_entry)) : NULL;
    populate_search_sidebar_with_mode(main_query, current_tab_is_full_text_search());
    populate_dict_sidebar();

}

static double shift_color_component(double val, double amount, int darken) {
    if (darken) return CLAMP(val - amount, 0.0, 1.0);
    return CLAMP(val + amount, 0.0, 1.0);
}

static void update_theme_colors(void) {
    if (!app_settings) return;
    gboolean is_default_theme = (g_strcmp0(app_settings->color_theme, "default") == 0);
    int dark_mode = adw_style_manager_get_dark(adw_style_manager_get_default()) ? 1 : 0;

    dsl_theme_palette palette;
    dict_render_get_theme_palette(app_settings->color_theme, dark_mode, &palette);

    GdkRGBA bg_color;
    if (!gdk_rgba_parse(&bg_color, palette.bg))
        gdk_rgba_parse(&bg_color, dark_mode ? "#1e1e21" : "#ffffff");

    /* Derive colors for JS injection, matching dict-render.c logic */
    char gold_surface[16], gold_badge[16];
    char slate_surface[16], slate_badge[16];
    char paper_surface[16], paper_edge[16], paper_accent[16];
    char tmp_color[16];

    if (dark_mode) {
        darken_hex_color(gold_surface, palette.link, sizeof(gold_surface), 0.28);
        darken_hex_color(gold_badge, palette.link, sizeof(gold_badge), 0.40);
        darken_hex_color(slate_surface, palette.border, sizeof(slate_surface), 0.82);
        lighten_hex_color(slate_badge, palette.border, sizeof(slate_badge));
        darken_hex_color(paper_surface, palette.com, sizeof(paper_surface), 0.30);
        darken_hex_color(paper_edge, palette.border, sizeof(paper_edge), 0.90);
        darken_hex_color(paper_accent, palette.link, sizeof(paper_accent), 0.72);
    } else {
        lighten_hex_color(tmp_color, palette.link, sizeof(tmp_color));
        lighten_hex_color(gold_surface, tmp_color, sizeof(gold_surface));
        g_strlcpy(gold_badge, tmp_color, sizeof(gold_badge));
        darken_hex_color(slate_surface, palette.bg, sizeof(slate_surface), 0.97);
        lighten_hex_color(slate_badge, palette.border, sizeof(slate_badge));
        lighten_hex_color(tmp_color, palette.com, sizeof(tmp_color));
        lighten_hex_color(paper_surface, tmp_color, sizeof(paper_surface));
        lighten_hex_color(paper_edge, palette.border, sizeof(paper_edge));
        lighten_hex_color(paper_accent, palette.link, sizeof(paper_accent));
    }

    char pbg[64], phov[64];
    if (dark_mode) {
        darken_hex_color(pbg, palette.link, sizeof(pbg), 0.55);
        darken_hex_color(phov, palette.link, sizeof(phov), 0.72);
    } else {
        darken_hex_color(pbg, palette.link, sizeof(pbg), 0.80);
        darken_hex_color(phov, palette.link, sizeof(phov), 0.65);
    }

    char *js_theme = g_strdup_printf(
        "if (typeof updateDictionTheme === 'function') {"
        "  updateDictionTheme({"
        "    'bg-color': '%s', 'body-color': '%s', 'link-color': '%s', 'heading-color': '%s', "
        "    'trn-color': '%s', 'ex-color': '%s', 'com-color': '%s', 'pos-color': '%s', "
        "    'translit-color': '%s', 'border-color': '%s', 'fts-highlight': '%s', "
        "    'gold-surface': '%s', 'gold-badge': '%s', "
        "    'slate-surface': '%s', 'slate-badge': '%s', "
        "    'paper-surface': '%s', 'paper-edge': '%s', 'paper-accent': '%s', "
        "    'code-bg': '%s', 'code-fg': '%s', 'table-border': '%s', "
        "    'pill-bg': '%s', 'pill-hover': '%s', "
        "    'xdxf-num': '%s', 'paper-shadow': '%s'"
        "  });"
        "}",
        palette.bg, palette.fg, palette.link, palette.heading,
        palette.trn, palette.ex, palette.com, palette.pos,
        palette.translit, palette.border, dark_mode ? "#ffd700" : "#ffeb3b",
        gold_surface, gold_badge, slate_surface, slate_badge,
        paper_surface, paper_edge, paper_accent,
        dark_mode ? "#242424" : "#f5f5f5", dark_mode ? "#ececec" : "#222222",
        dark_mode ? "#555555" : "#d0d0d0", pbg, phov,
        dark_mode ? "#ff6b6b" : "#d32f2f",
        dark_mode ? "0 1px 0 rgba(0,0,0,0.22)" : "none");

    if (tab_view) {
        GListModel *pages = G_LIST_MODEL(adw_tab_view_get_pages(tab_view));
        guint n_pages = g_list_model_get_n_items(pages);
        for (guint i = 0; i < n_pages; i++) {
            AdwTabPage *page = ADW_TAB_PAGE(g_list_model_get_item(pages, i));
            GtkWidget *scroll = adw_tab_page_get_child(page);
            WebKitWebView *wv = get_web_view_from_scroll(scroll);
            if (wv) {
                webkit_web_view_set_background_color(wv, &bg_color);
                webkit_web_view_evaluate_javascript(wv, js_theme, -1, NULL, NULL, NULL, NULL, NULL);
            }
        }
    }
    g_free(js_theme);

    if (!dynamic_theme_provider) {
        dynamic_theme_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(dynamic_theme_provider),
            GTK_STYLE_PROVIDER_PRIORITY_USER   /* 800 — beats Adwaita at 600 */
        );
    }

    /* Derive Chrome/Surface colors inspired by ViTE */
    double r, g, b;
    unsigned int ir, ig, ib;
    sscanf(palette.bg + 1, "%02x%02x%02x", &ir, &ig, &ib);
    r = ir / 255.0; g = ig / 255.0; b = ib / 255.0;

    /* Chrome (Sidebar/Header): slightly shifted (darker in light, lighter in dark) */
    double shift1 = 0.03;
    char c_chrome[32];
    g_snprintf(c_chrome, sizeof(c_chrome), "rgb(%d,%d,%d)",
               (int)(shift_color_component(r, shift1, !dark_mode) * 255),
               (int)(shift_color_component(g, shift1, !dark_mode) * 255),
               (int)(shift_color_component(b, shift1, !dark_mode) * 255));

    /* Surface (Popovers): shifted more */
    double shift2 = 0.06;
    char c_surface[32];
    g_snprintf(c_surface, sizeof(c_surface), "rgb(%d,%d,%d)",
               (int)(shift_color_component(r, shift2, !dark_mode) * 255),
               (int)(shift_color_component(g, shift2, !dark_mode) * 255),
               (int)(shift_color_component(b, shift2, !dark_mode) * 255));

    /* Compute hover rgba manually (alpha() is GTK3-only syntax) */
    unsigned int ar = 0x33, ag = 0x99, ab = 0xcc;
    if (palette.accent && palette.accent[0] == '#' && strlen(palette.accent) >= 7)
        sscanf(palette.accent + 1, "%02x%02x%02x", &ar, &ag, &ab);
    char hover_color[64];
    if (is_default_theme) {
        g_strlcpy(hover_color, dark_mode ? "rgba(255, 255, 255, 0.06)" : "rgba(0, 0, 0, 0.05)", sizeof(hover_color));
    } else {
        g_snprintf(hover_color, sizeof(hover_color), "rgba(%u,%u,%u,0.15)", ar, ag, ab);
    }
    char select_color[32];
    g_snprintf(select_color, sizeof(select_color),
               "rgba(%u,%u,%u,0.25)", ar, ag, ab);

    /*
     * Use Adwaita's @define-color mechanism so the theme engine picks up
     * our palette for its own rules (borders, shadows, transitions, etc.).
     * Direct property overrides are added below as a belt-and-suspenders
     * fallback, but @define-color is what actually moves the needle.
     */
    /* is_default_theme declared above */

    const char *w_bg = (is_default_theme) ? (dark_mode ? "#1e1e21" : "#ffffff") : palette.bg;
    const char *h_bg = (is_default_theme) ? (dark_mode ? "#1e1e21" : "#ffffff") : palette.bg;
    const char *ch_bg = (is_default_theme) ? (dark_mode ? "#1e1e21" : "#ffffff") : palette.bg;
    const char *w_fg = (is_default_theme) ? (dark_mode ? "#ffffff" : "#222222") : palette.fg;
    const char *sidebar_bg = (is_default_theme) ? (dark_mode ? "#2e2e32" : "#f6f6f6") : c_chrome;
    const char *accent = (is_default_theme) ? (dark_mode ? "#3584e4" : "#e45649") : palette.accent;
    const char *popover_bg = (is_default_theme) ? (dark_mode ? "#2e2e32" : "#ffffff") : c_surface;

    /* Selection colors: standard neutral grey for hover, soft tint for activation */
    char sidebar_hover[64];
    g_strlcpy(sidebar_hover, "rgba(128, 128, 128, 0.15)", sizeof(sidebar_hover));

    char sidebar_selected_bg[64];
    char sidebar_selected_fg[32];
    if (dark_mode) {
        g_strlcpy(sidebar_selected_bg, "rgba(255, 255, 255, 0.14)", sizeof(sidebar_selected_bg));
        g_strlcpy(sidebar_selected_fg, "#ffffff", sizeof(sidebar_selected_fg));
    } else {
        g_strlcpy(sidebar_selected_bg, is_default_theme ? "#ffffff" : "rgba(0, 0, 0, 0.07)",
                  sizeof(sidebar_selected_bg));
        g_strlcpy(sidebar_selected_fg, "#202124", sizeof(sidebar_selected_fg));
    }

    char *css = g_strdup_printf(
        "@define-color window_bg_color %s;\n"
        "@define-color window_fg_color %s;\n"
        "@define-color view_bg_color %s;\n"
        "@define-color view_fg_color %s;\n"
        "@define-color headerbar_bg_color %s;\n"
        "@define-color headerbar_fg_color %s;\n"
        "@define-color headerbar_border_color transparent;\n"
        "@define-color sidebar_bg_color %s;\n"
        "@define-color sidebar_fg_color %s;\n"
        "@define-color popover_bg_color %s;\n"
        "@define-color popover_fg_color %s;\n"
        "@define-color accent_bg_color %s;\n"
        "@define-color accent_fg_color #ffffff;\n"
        "\n"
        "/* Standard search entry selection */\n"
        "selection { background-color: @accent_bg_color; color: @accent_fg_color; }\n"
        "entry selection { background-color: @accent_bg_color; color: @accent_fg_color; }\n"
        "\n"
        "window.background {\n"
        "  background-color: @window_bg_color;\n"
        "  color: @window_fg_color;\n"
        "}\n"
        "headerbar {\n"
        "  background-color: @headerbar_bg_color;\n"
        "  color: @headerbar_fg_color;\n"
        "  border-bottom: none;\n"
        "}\n"
        ".sidebar, .navigation-sidebar, .sidebar listview, .navigation-sidebar listview, .sidebar list, .navigation-sidebar list, .sidebar scrolledwindow, .navigation-sidebar scrolledwindow {\n"
        "  background-color: @sidebar_bg_color;\n"
        "  border: none;\n"
        "}\n"
        ".navigation-sidebar {\n"
        "  border-right: 1px solid %s;\n"
        "}\n"
        ".sidebar {\n"
        "  border: none;\n"
        "}\n"
        "/* Navigation sidebar item styling - user prefers opacity over color for selection */\n"
        ".navigation-sidebar row, .navigation-sidebar listitem {\n"
        "  margin: 2px 8px;\n"
        "  padding: 6px 10px;\n"
        "  border-radius: 8px;\n"
        "  transition: background-color 150ms ease-out, color 150ms ease-out;\n"
        "}\n"
        "row, listitem {\n"
        "  color: inherit;\n"
        "}\n"
        "row:selected, row:selected:focus, row:selected:focus-within, row:selected:backdrop,\n"
        "listitem:selected, listitem:selected:focus, listitem:selected:focus-within, listitem:selected:backdrop,\n"
        "listview listitem:selected, listview listitem:selected:backdrop {\n"
        "  background-color: %s;\n"
        "  color: %s;\n"
        "  outline: none;\n"
        "  transition: none;\n"
        "}\n"
        "listitem:selected .sidebar-row, listitem:selected:backdrop .sidebar-row,\n"
        "row:selected .sidebar-row, row:selected:backdrop .sidebar-row {\n"
        "  background-color: transparent;\n"
        "  color: %s;\n"
        "}\n"

        "/* Explicitly set webview backgrounds */\n"
        "webkitwebview, webview {\n"
        "  background-color: @view_bg_color;\n"
        "}\n"
        ".hw-hovered {\n"
        "  background-color: %s;\n"
        "  color: inherit;\n"
        "}\n"
        "popover, popovermenu {\n"
        "  background-color: transparent;\n"
        "}\n"
        "popover > contents, popovermenu > contents {\n"
        "  background-color: @popover_bg_color;\n"
        "  color: @popover_fg_color;\n"
        "  padding: 0;\n"
        "  border-radius: 12px;\n"
        "}\n"
        "/* Popup menu item styling */\n"
        "popover > contents row,\n"
        "popover > contents listitem {\n"
        "  color: inherit;\n"
        "  background-color: transparent;\n"
        "}\n"
        "popover > contents row:hover:not(:selected),\n"
        "popover > contents listitem:hover:not(:selected) {\n"
        "  background-color: %s;\n"
        "}\n"
        "popover > contents row:checked {\n"
        "  color: @accent_bg_color;\n"
        "}\n"
        "/* About dialog styles */\n"
        "window.about label.version {\n"
        "  opacity: 0.6;\n"
        "}\n"
        "label.dim-label, .dim-label {\n"
        "  opacity: 0.7;\n"
        "}\n"
        ".content-header, .content-header headerbar {\n"
        "  background-color: transparent;\n"
        "  border-bottom: none;\n"
        "  box-shadow: none;\n"
        "}\n"
        ".article-view-container, adwtoolbarview > stack {\n"
        "  background-color: @view_bg_color;\n"
        "}\n"
        "tabbar, .tab-bar, tabbar box {\n"
        "  background-color: %s;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "  border-style: none;\n"
        "  border-bottom: none;\n"
        "}\n"
        "adwtoolbarview separator, adwtoolbarview > stack > separator {\n"
        "  min-height: 0;\n"
        "  opacity: 0;\n"
        "  background: transparent;\n"
        "}\n"
        /* hw-selected: beat the theme's row:selected/row:hover rules above */
        "listview row.hw-selected,"
        "listview row.hw-selected:hover,"
        "listview row.hw-selected:selected,"
        "listview row.hw-selected:selected:hover,"
        "listview listitem.hw-selected,"
        "listview listitem.hw-selected:hover,"
        "listview listitem.hw-selected:selected,"
        "listview listitem.hw-selected:selected:hover {"
        "  background-color: alpha(@accent_bg_color, 0.22);"
        "  color: inherit;"
        "}"
        "listview row.hw-selected .sidebar-row,"
        "listview row.hw-selected:hover .sidebar-row,"
        "listview listitem.hw-selected .sidebar-row,"
        "listview listitem.hw-selected:hover .sidebar-row {"
        "  background-color: transparent;"
        "  color: inherit;"
        "}"
        "listview row:not(.hw-selected):selected,"
        "listview listitem:not(.hw-selected):selected {"
        "  background-color: transparent;"
        "  color: inherit;"
        "}"
        /* Hover highlight: MUST come last to beat :selected clearing rules */
        "listview row:hover:not(.hw-selected),"
        "listview row:not(.hw-selected):selected:hover,"
        "listview listitem:hover:not(.hw-selected),"
        "listview listitem:not(.hw-selected):selected:hover {"
        "  background-color: %s;"
        "  color: inherit;"
        "}\n",
        /* @define-color values */
        w_bg, w_fg, ch_bg, w_fg, h_bg, w_fg,
        sidebar_bg, w_fg, popover_bg, w_fg, accent,
        /* sidebar border (1) */
        palette.border,
        /* selected item colors (3) */
        sidebar_selected_bg, sidebar_selected_fg, sidebar_selected_fg,
        /* .hw-hovered (1) */
        sidebar_hover,
        /* popover row hover (1) */
        hover_color,
        /* tab bar (1) */
        ch_bg,
        /* listview row:hover — last rule in CSS (1) */
        sidebar_hover
    );

    gtk_css_provider_load_from_string(dynamic_theme_provider, css);
    g_free(css);
}

static void on_style_manager_changed(AdwStyleManager *manager, GParamSpec *pspec, gpointer user_data) {
    (void)manager; (void)pspec; (void)user_data;
    apply_font_to_webview(NULL);
}

/* Called whenever font family or size changes in the Appearance tab */
static void apply_font_to_webview(void *user_data) {
    (void)user_data;
    if (!app_settings) return;

    /* Get current dark mode state */
    int dark_mode = adw_style_manager_get_dark(adw_style_manager_get_default()) ? 1 : 0;
    dsl_theme_palette palette;
    dict_render_get_theme_palette(app_settings->color_theme, dark_mode, &palette);
    gboolean is_default_theme = (g_strcmp0(app_settings->color_theme, "default") == 0);

    /* Update WebKit settings for ALL tabs */
    if (tab_view) {
        GListModel *pages = G_LIST_MODEL(adw_tab_view_get_pages(tab_view));
        guint n_pages = g_list_model_get_n_items(pages);
        for (guint i = 0; i < n_pages; i++) {
            AdwTabPage *page = ADW_TAB_PAGE(g_list_model_get_item(pages, i));
            GtkWidget *scroll = adw_tab_page_get_child(page);
            WebKitWebView *wv = get_web_view_from_scroll(scroll);
            if (wv) {
                WebKitSettings *web_settings = webkit_web_view_get_settings(wv);
                if (app_settings->font_family && *app_settings->font_family)
                    webkit_settings_set_default_font_family(web_settings, app_settings->font_family);
                if (app_settings->font_size > 0)
                    webkit_settings_set_default_font_size(web_settings, (guint32)app_settings->font_size);
            }
        }
    }

    /* Inject / replace a user stylesheet that forces the font on every
     * element with !important.  This overrides any CSS the page itself has,
     * including MDX dictionaries that ship their own stylesheets. */
    if (font_ucm) {
        if (font_user_stylesheet) {
            webkit_user_content_manager_remove_style_sheet(font_ucm, font_user_stylesheet);
            webkit_user_style_sheet_unref(font_user_stylesheet);
            font_user_stylesheet = NULL;
        }

        const char *ff = (app_settings->font_family && *app_settings->font_family)
                         ? app_settings->font_family : "sans-serif";
        char css[1024];

        if (app_settings->font_size > 0) {
            /* Use em-based size override so relative sizes within the page
             * still scale correctly (1em = our chosen px at root level). */
            if (strchr(ff, ' ') && ff[0] != '\"' && ff[0] != '\'')
                g_snprintf(css, sizeof(css),
                    "* { font-family: \"%s\", sans-serif !important; }"
                    "body { font-size: %dpx !important; }"
                    "::selection { background-color: %s !important; color: inherit !important; }\n"
                    "::-webkit-selection { background-color: %s !important; color: inherit !important; }\n"
                    "::selection:inactive { background-color: %s !important; color: inherit !important; }\n"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid %s !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }",
                    ff, app_settings->font_size, 
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.2)" : "rgba(127, 127, 255, 0.15)",
                    palette.accent);
            else
                g_snprintf(css, sizeof(css),
                    "* { font-family: %s, sans-serif !important; }"
                    "body { font-size: %dpx !important; }"
                    "::selection { background-color: %s !important; color: inherit !important; }\n"
                    "::-webkit-selection { background-color: %s !important; color: inherit !important; }\n"
                    "::selection:inactive { background-color: %s !important; color: inherit !important; }\n"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid %s !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }",
                    ff, app_settings->font_size,
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.2)" : "rgba(127, 127, 255, 0.15)",
                    palette.accent);
        } else {
            if (strchr(ff, ' ') && ff[0] != '\"' && ff[0] != '\'')
                g_snprintf(css, sizeof(css),
                    "* { font-family: \"%s\", sans-serif !important; }"
                    "::selection { background-color: %s !important; color: inherit !important; }\n"
                    "::-webkit-selection { background-color: %s !important; color: inherit !important; }\n"
                    "::selection:inactive { background-color: %s !important; color: inherit !important; }\n"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid %s !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }",
                    ff, 
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.2)" : "rgba(127, 127, 255, 0.15)",
                    palette.accent);
            else
                g_snprintf(css, sizeof(css),
                    "* { font-family: %s, sans-serif !important; }"
                    "::selection { background-color: %s !important; color: inherit !important; }\n"
                    "::-webkit-selection { background-color: %s !important; color: inherit !important; }\n"
                    "::selection:inactive { background-color: %s !important; color: inherit !important; }\n"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid %s !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }",
                    ff, 
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.4)" : "rgba(127, 127, 255, 0.25)",
                    (is_default_theme) ? "rgba(100, 150, 255, 0.2)" : "rgba(127, 127, 255, 0.15)",
                    palette.accent);
        }

        font_user_stylesheet = webkit_user_style_sheet_new(
            css,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_STYLE_LEVEL_USER,
            NULL, NULL);
        webkit_user_content_manager_add_style_sheet(font_ucm, font_user_stylesheet);
    }

    /* Refresh UI colors and WebKit background to match new theme/font state */
    update_theme_colors();
}

static void on_web_view_load_changed(WebKitWebView *wv,
                                     WebKitLoadEvent load_event,
                                     gpointer user_data) {
    (void)user_data;

    if (load_event != WEBKIT_LOAD_FINISHED) {
        return;
    }

    const char *pending_query =
        g_object_get_data(G_OBJECT(wv), "pending-fts-highlight-query");
    if (!pending_query || !*pending_query) {
        return;
    }

    apply_fts_highlight_to_web_view(wv, pending_query);
    g_object_set_data(G_OBJECT(wv), "pending-fts-highlight-query", NULL);
}




static void reload_dictionaries_from_settings(void *user_data);

static gboolean reload_dictionaries_from_settings_idle(gpointer user_data) {
    (void)user_data;
    dictionary_watch_reload_source_id = 0;
    reload_dictionaries_from_settings(NULL);
    return G_SOURCE_REMOVE;
}

void force_next_dictionary_directory_rescan(void) {
    force_directory_rescan_requested = TRUE;
}

static void request_dictionary_directory_rescan(gboolean force_directory_rescan) {
    if (force_directory_rescan) {
        force_next_dictionary_directory_rescan();
    }

    if (dictionary_watch_reload_source_id != 0) {
        return;
    }

    dictionary_watch_reload_source_id = g_timeout_add(600, reload_dictionaries_from_settings_idle, NULL);
}

static void reload_dictionaries_from_settings(void *user_data) {
    (void)user_data;
    gboolean discover_from_dirs =
        force_directory_rescan_requested || should_rescan_dictionary_dirs();
    force_directory_rescan_requested = FALSE;
    startup_random_word_pending = FALSE;
    g_atomic_int_inc(&loader_generation);
    refresh_dictionary_directory_monitors();
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }
    cancel_sidebar_search();

    // Clear sidebar and transient state
    clear_related_rows();

    dictionary_loading_in_progress = FALSE;

    // Show "Reloading..." and start async scan
    render_idle_page_to_webview(web_view, "Reloading dictionaries...", "Please wait.");

    if (!active_scope_id) {
        active_scope_id = g_strdup("all");
    }
    rebuild_dict_entries_from_settings();
    populate_dict_sidebar();
    populate_history_sidebar();
    populate_favorites_sidebar();

    populate_search_sidebar(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    if (!start_async_dict_loading(discover_from_dirs)) {
        finalize_dictionary_loading(FALSE, discover_from_dirs);
    }
}

/* Request cancellation of the current loader generation (called from UI). */

static void finalize_dictionary_loading(gboolean allow_random_word, gboolean sync_settings_from_loaded) {
    dictionary_loading_in_progress = FALSE;
    if (sync_settings_from_loaded) {
        sync_settings_dictionaries_from_loaded();
    }
    rebuild_dict_entries_from_settings();
    
    /* Clean up any orphaned dictionary cache files or FTS SQLite databases */
    GPtrArray *active_paths = g_ptr_array_new();
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (e->path) {
            g_ptr_array_add(active_paths, (gpointer)e->path);
        }
    }
    dict_cache_garbage_collect(active_paths);
    g_ptr_array_free(active_paths, TRUE);

    extern void settings_scan_notify(const char *name, const char *path, int event_type);
    settings_scan_notify(NULL, NULL, -1);
    if (!active_entry && all_dicts) {
        set_active_entry(all_dicts);
    }
    populate_dict_sidebar();

    populate_history_sidebar();
    populate_favorites_sidebar();
    rebuild_search_scope_menu();
    update_search_scope_button_label();
    populate_search_sidebar(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    refresh_search_results();

    if (startup_splash_is_active()) {
        startup_splash_close();
        if (main_window) {
            gtk_window_present(main_window);
        }
    }

    if (!all_dicts) {
        webkit_web_view_load_html(web_view,
            "<h2>No Dictionaries Found</h2>"
            "<p>Open <b>Preferences</b> and add a dictionary directory.</p>",
            "file:///");
    } else if (allow_random_word) {
        maybe_show_startup_random_word();
        const char *current = gtk_editable_get_text(GTK_EDITABLE(search_entry));
        if (current && strlen(current) == 0) {
            on_random_clicked(NULL, NULL);
        }
    }
}

static void toggle_scan_from_tray(void) {
    if (app_settings) {
        app_settings->scan_popup_enabled = !app_settings->scan_popup_enabled;
        scan_popup_set_enabled(app_settings->scan_popup_enabled);
        tray_icon_set_scan_active(app_settings->scan_popup_enabled);
        settings_save(app_settings);
    }
}

static void quit_from_tray(void) {
    GApplication *app = g_application_get_default();
    if (app) {
        g_application_quit(app);
    }
}

static void refresh_dictionaries_ui(void *user_data) {
    (void)user_data;
    populate_dict_sidebar();
    populate_history_sidebar();
    populate_favorites_sidebar();
    rebuild_search_scope_menu();
    update_search_scope_button_label();

    if (search_entry) {
        populate_search_sidebar(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    }
    refresh_search_results();

    if (app_settings) {
        if (app_settings->tray_icon_enabled) {
            tray_icon_init(GTK_APPLICATION(g_application_get_default()), 
                           main_window, app_show_window, toggle_scan_from_tray, quit_from_tray);
            tray_icon_set_scan_active(app_settings->scan_popup_enabled);
        } else {
            tray_icon_destroy();
        }
        scan_popup_set_enabled(app_settings->scan_popup_enabled);
    }
}

static void on_scan_clipboard_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    scan_popup_trigger_manual();
}

static void reveal_search_entry(gboolean select_text) {
    if (!search_stack || !search_entry) {
        return;
    }

    gtk_stack_set_visible_child_name(search_stack, "entry");
    gtk_widget_grab_focus(GTK_WIDGET(search_entry));
    update_search_mode_visuals(current_tab_is_full_text_search());

    if (select_text) {
        gtk_editable_select_region(GTK_EDITABLE(search_entry), 0, -1);
    }
}

static void on_focus_search_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    reveal_search_entry(TRUE);
}

static void on_search_scope_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)user_data;
    if (!parameter) {
        return;
    }

    const char *scope_id = g_variant_get_string(parameter, NULL);
    set_active_scope(scope_id, TRUE);
}

static void on_full_text_search_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;

    AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
    if (!search_entry || !page) {
        return;
    }

    gboolean enable_fts = !tab_page_is_full_text_search(page);
    set_tab_full_text_search(page, enable_fts);

    /* Apply icon + placeholder immediately — before reveal_search_entry so
     * the user sees the correct state as soon as the entry appears. */
    update_search_mode_visuals(enable_fts);

    reveal_search_entry(TRUE);
    g_clear_pointer(&fts_highlight_query, g_free);

    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }

    cancel_sidebar_search();

    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    char *clean_query = normalize_headword_for_search(query, FALSE);
    if (!clean_query || !*clean_query) {
        if (enable_fts) {
            populate_search_sidebar_status("Full Text Search", "Type a word or phrase to search definitions in this scope.");
        } else if (query && *query) {
            populate_search_sidebar(query);
        }
        g_free(clean_query);
        return;
    }

    g_free(clean_query);
    if (!enable_fts) {
        populate_search_sidebar(query);
    }
}

static void set_current_web_view_zoom(double level) {
    WebKitWebView *wv = get_current_web_view();
    if (!wv) {
        return;
    }

    webkit_web_view_set_zoom_level(wv, CLAMP(level, 0.5, 3.0));
}

static void adjust_current_web_view_zoom(double delta) {
    WebKitWebView *wv = get_current_web_view();
    if (!wv) {
        return;
    }

    set_current_web_view_zoom(webkit_web_view_get_zoom_level(wv) + delta);
}

static void on_zoom_in_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    adjust_current_web_view_zoom(0.1);
}

static void on_zoom_out_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    adjust_current_web_view_zoom(-0.1);
}

static void on_zoom_reset_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    set_current_web_view_zoom(1.0);
}

static void show_settings_dialog(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(user_data));
    if (window) {
        GtkWidget *dialog = settings_dialog_new(window, app_settings, style_manager,
            reload_dictionaries_from_settings, refresh_dictionaries_ui, NULL);
        settings_dialog_set_font_callback(dialog, apply_font_to_webview, NULL);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
    }
}

static void show_about_dialog(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(user_data));
    if (window) {
        AdwDialog *dialog = adw_about_dialog_new();
        adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), "Diction");
        adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog), "io.github.fastrizwaan.diction");
        adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(dialog), "A high-performance, multi-format offline dictionary.");
        adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), "0.1.0");
        adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(dialog), "Mohammed Asif Ali Rizvan");
        adw_about_dialog_set_website(ADW_ABOUT_DIALOG(dialog), "https://github.com/fastrizwaan/diction");
        adw_about_dialog_set_copyright(ADW_ABOUT_DIALOG(dialog), "© 2024 Mohammed Asif Ali Rizvan");
        adw_about_dialog_set_license(ADW_ABOUT_DIALOG(dialog), "GPL-3.0-or-later");
        adw_dialog_present(dialog, GTK_WIDGET(window));
    }
}

static void on_sidebar_tab_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (gtk_toggle_button_get_active(btn)) {
        AdwViewStack *stack = g_object_get_data(G_OBJECT(btn), "stack-widget");
        const char *name = g_object_get_data(G_OBJECT(btn), "stack-name");
        if (stack && name) {
            adw_view_stack_set_visible_child_name(stack, name);
        }
    }
}

static void populate_dict_sidebar(void) {
    ensure_valid_active_scope();
    ensure_active_entry_in_scope();

    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *payloads = g_ptr_array_new();

    g_mutex_lock(&dict_loader_mutex);
    DictEntry *e = all_dicts;
    while (e) {
        dict_entry_ref(e);
        g_mutex_unlock(&dict_loader_mutex);

        if (dict_entry_visible_in_sidebar(e)) {
            SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
            payload->type = SIDEBAR_ROW_DICT;
            payload->title = g_strdup(e->name ? e->name : "Dictionary");
            payload->dict_entry = e;
            dict_entry_ref(e);
            if (e->icon_path) {
                payload->icon_path = g_strdup(e->icon_path);
            }
            g_ptr_array_add(labels, g_strdup(payload->title));
            g_ptr_array_add(payloads, payload);
        }

        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = e->next;
        dict_entry_unref(e);
        e = next;
    }
    g_mutex_unlock(&dict_loader_mutex);

    if (labels->len == 0) {
        SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
        payload->type = SIDEBAR_ROW_HINT;
        payload->title = g_strdup("No dictionaries");
        payload->subtitle = g_strdup("Load or enable a dictionary to browse results.");
        g_ptr_array_add(labels, g_strdup(payload->title));
        g_ptr_array_add(payloads, payload);
    }

    set_sidebar_list_rows(&dict_sidebar, labels, payloads);

    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);
}

// ------- Async loading infrastructure -------

typedef enum {
    LOAD_IDLE_STATUS = 0,
    LOAD_IDLE_ENTRY,
    LOAD_IDLE_DONE
} LoadIdleKind;

typedef struct {
    char **dirs;          // NULL-terminated array of directory paths to scan
    int   n_dirs;
    char **manual_paths;  // NULL-terminated array of manually-added dictionary files
    int   n_manual;
    GHashTable *ignored_paths;
    gint  generation;
    gboolean discover_from_dirs;
} LoadThreadArgs;

typedef struct {
    LoadIdleKind kind;
    DictEntry *entry; // single loaded entry (next == NULL on delivery)
    gint       generation;
    guint      completed;
    guint      total;
    char      *status_text;
    gboolean   sync_settings;
} LoadIdleData;

static gboolean loader_path_ends_with_ci(const char *path, const char *suffix) {
    gsize path_len = path ? strlen(path) : 0;
    gsize suffix_len = suffix ? strlen(suffix) : 0;
    if (!path || !suffix || path_len < suffix_len) {
        return FALSE;
    }

    return g_ascii_strcasecmp(path + path_len - suffix_len, suffix) == 0;
}

static gboolean loader_is_dsl_family_path(const char *path) {
    return loader_path_ends_with_ci(path, ".dsl") || loader_path_ends_with_ci(path, ".dsl.dz");
}

static char *loader_dsl_family_key(const char *path) {
    if (loader_path_ends_with_ci(path, ".dsl.dz")) {
        return g_strndup(path, strlen(path) - 3);
    }
    return g_strdup(path);
}

static char *loader_dsl_preferred_variant(const char *path) {
    if (loader_path_ends_with_ci(path, ".dsl.dz")) {
        return g_strdup(path);
    }
    if (loader_path_ends_with_ci(path, ".dsl")) {
        char *compressed = g_strconcat(path, ".dz", NULL);
        if (g_file_test(compressed, G_FILE_TEST_EXISTS)) {
            return compressed;
        }
        g_free(compressed);
    }
    return g_strdup(path);
}

static void loader_add_candidate_path(const char *path,
                                      GPtrArray *out_paths,
                                      GHashTable *seen_paths,
                                      GHashTable *seen_dsl_families,
                                      GHashTable *ignored_paths) {
    if (!path || !out_paths) {
        return;
    }

    DictFormat fmt = dict_detect_format(path);
    if (fmt == DICT_FORMAT_UNKNOWN) {
        return;
    }

    char *load_path = NULL;
    char *family_key = NULL;

    if (fmt == DICT_FORMAT_DSL && loader_is_dsl_family_path(path)) {
        family_key = loader_dsl_family_key(path);
        if (seen_dsl_families && g_hash_table_contains(seen_dsl_families, family_key)) {
            g_free(family_key);
            return;
        }
        load_path = loader_dsl_preferred_variant(path);
    } else {
        load_path = g_strdup(path);
    }

    if ((ignored_paths && g_hash_table_contains(ignored_paths, load_path)) ||
        (seen_paths && g_hash_table_contains(seen_paths, load_path))) {
        g_free(load_path);
        g_free(family_key);
        return;
    }

    if (seen_dsl_families && family_key) {
        g_hash_table_add(seen_dsl_families, family_key);
        family_key = NULL;
    }

    if (seen_paths) {
        g_hash_table_add(seen_paths, g_strdup(load_path));
    }

    g_ptr_array_add(out_paths, load_path);
    g_free(family_key);
}

extern void settings_scan_notify(const char *name, const char *path, int event_type);

static void collect_dictionary_candidate_paths_with_find(const char *dirpath,
                                                        GPtrArray *out_paths,
                                                        GHashTable *seen_paths,
                                                        GHashTable *seen_dsl_families,
                                                        GHashTable *ignored_paths,
                                                        gint generation,
                                                        GCancellable *cancellable) {
    if (!dirpath || !out_paths) return;

    char *expanded = NULL;
    if (dirpath[0] == '~') {
        expanded = g_build_filename(g_get_home_dir(), dirpath + 1, NULL);
    } else {
        expanded = g_strdup(dirpath);
    }

    /* Use GSubprocess to run 'find'. Skip heavy folders. 
     * Use -iname for case-insensitivity and -maxdepth 5. */
    GPtrArray *argv_array = g_ptr_array_new();
    g_ptr_array_add(argv_array, g_strdup("find"));
    g_ptr_array_add(argv_array, g_strdup(expanded));
    g_ptr_array_add(argv_array, g_strdup("-maxdepth"));
    g_ptr_array_add(argv_array, g_strdup("5"));
    g_ptr_array_add(argv_array, g_strdup("-type"));
    g_ptr_array_add(argv_array, g_strdup("f"));
    g_ptr_array_add(argv_array, g_strdup("("));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.mdx"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.dsl"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.dsl.dz"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.ifo"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.bgl"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.slob"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.xdxf"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.xdxf.dz"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.tar.bz2"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.tar.gz"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.tar.xz"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.tgz"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.zip"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.index"));
    g_ptr_array_add(argv_array, g_strdup("-o"));
    g_ptr_array_add(argv_array, g_strdup("-iname")); g_ptr_array_add(argv_array, g_strdup("*.dct"));
    g_ptr_array_add(argv_array, g_strdup(")"));
    g_ptr_array_add(argv_array, g_strdup("-not")); g_ptr_array_add(argv_array, g_strdup("-path")); g_ptr_array_add(argv_array, g_strdup("*/node_modules/*"));
    g_ptr_array_add(argv_array, g_strdup("-not")); g_ptr_array_add(argv_array, g_strdup("-path")); g_ptr_array_add(argv_array, g_strdup("*/.git/*"));
    g_ptr_array_add(argv_array, g_strdup("-not")); g_ptr_array_add(argv_array, g_strdup("-path")); g_ptr_array_add(argv_array, g_strdup("*/build/*"));
    g_ptr_array_add(argv_array, g_strdup("-not")); g_ptr_array_add(argv_array, g_strdup("-path")); g_ptr_array_add(argv_array, g_strdup("*/dist/*"));
    g_ptr_array_add(argv_array, g_strdup("-not")); g_ptr_array_add(argv_array, g_strdup("-path")); g_ptr_array_add(argv_array, g_strdup("*/__pycache__/*"));
    g_ptr_array_add(argv_array, NULL);

    gchar **argv = (gchar **)g_ptr_array_free(argv_array, FALSE);
    
    GError *err = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    GSubprocess *sub = g_subprocess_launcher_spawnv(launcher, (const gchar * const *)argv, &err);
    g_strfreev(argv);
    g_object_unref(launcher);
    
    g_free(expanded);
    
    if (!sub) {
        if (err) {
            fprintf(stderr, "[LOADER] GSubprocess failed: %s\n", err->message);
            g_error_free(err);
        }
        return;
    }

    GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(sub);
    GDataInputStream *dstream = g_data_input_stream_new(stdout_stream);

    int count = 0;
    char *line = NULL;
    gsize length = 0;
    while ((line = g_data_input_stream_read_line_utf8(dstream, &length, cancellable, NULL)) != NULL) {
        if (generation != g_atomic_int_get(&loader_generation)) {
            g_free(line);
            break;
        }
        if (g_cancellable_is_cancelled(cancellable)) {
            g_free(line);
            break;
        }
        if (line[0] == '\0') {
            g_free(line);
            continue;
        }

        count++;
        guint before = out_paths->len;
        loader_add_candidate_path(line, out_paths, seen_paths, seen_dsl_families, ignored_paths);
        if (out_paths->len > before) {
            const char *new_path = g_ptr_array_index(out_paths, out_paths->len - 1);
            char *b = g_path_get_basename(new_path);
            settings_scan_notify(b, new_path, DICT_LOADER_EVENT_DISCOVERED);
            g_free(b);
        }
        g_free(line);
    }

    /* Force kill the subprocess so it doesn't linger reading a dead mount */
    g_subprocess_force_exit(sub);
    g_object_unref(dstream);
    g_object_unref(sub);

    fprintf(stderr, "[LOADER] Discovery found %d raw candidates\n", count);
}

static void collect_dictionary_candidate_paths_recursive(const char *dirpath,
                                                         GPtrArray *out_paths,
                                                         GHashTable *seen_paths,
                                                         GHashTable *seen_dsl_families,
                                                         GHashTable *ignored_paths,
                                                         gint generation,
                                                         int depth) {
    if (!dirpath || !out_paths || depth > 5) {
        return;
    }
    /* Fallback to C recursive scanner if find is not used or fails */
    if (generation != g_atomic_int_get(&loader_generation)) return;

    char *expanded = NULL;
    if (dirpath[0] == '~') {
        expanded = g_build_filename(g_get_home_dir(), dirpath + 1, NULL);
    } else {
        expanded = g_strdup(dirpath);
    }

    GDir *dir = g_dir_open(expanded, 0, NULL);
    g_free(expanded);
    if (!dir) return;

    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.') continue;
        if (generation != g_atomic_int_get(&loader_generation)) break;

        char *full = g_build_filename(dirpath, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            if (!loader_path_ends_with_ci(full, ".files") &&
                !loader_path_ends_with_ci(full, ".dsl.files") &&
                !loader_path_ends_with_ci(full, ".dsl.dz.files") &&
                g_ascii_strcasecmp(name, "node_modules") != 0 &&
                g_ascii_strcasecmp(name, "build") != 0 &&
                g_ascii_strcasecmp(name, "dist") != 0 &&
                g_ascii_strcasecmp(name, "vendor") != 0 &&
                g_ascii_strcasecmp(name, "__pycache__") != 0) {
                collect_dictionary_candidate_paths_recursive(full, out_paths,
                                                            seen_paths, seen_dsl_families,
                                                            ignored_paths, generation, depth + 1);
            }
        } else {
            guint before = out_paths->len;
            loader_add_candidate_path(full, out_paths, seen_paths, seen_dsl_families, ignored_paths);
            if (out_paths->len > before) {
                const char *new_path = g_ptr_array_index(out_paths, out_paths->len - 1);
                char *b = g_path_get_basename(new_path);
                settings_scan_notify(b, new_path, DICT_LOADER_EVENT_DISCOVERED);
                g_free(b);
            }
        }
        g_free(full);
    }
    g_dir_close(dir);
}

static DictEntry *create_dict_entry_from_loaded(const char *path, DictFormat fmt, DictMmap *dict) {
    if (!path || !dict) {
        return NULL;
    }

    DictEntry *entry = g_new0(DictEntry, 1);
    entry->magic = 0xDEADC0DE;
    entry->ref_count = 1;
    entry->format = fmt;
    entry->dict = dict;

    if (dict->name && *dict->name) {
        char *valid = g_utf8_make_valid(dict->name, -1);
        entry->name = g_strdup(valid);
        g_free(valid);
    } else {
        char *base = g_path_get_basename(path);
        char *valid = g_utf8_make_valid(base, -1);
        entry->name = g_strdup(valid);
        g_free(valid);
        g_free(base);
    }

    char *valid_path = g_utf8_make_valid(path, -1);
    entry->path = g_strdup(valid_path);
    g_free(valid_path);
    entry->dict_id = settings_make_dictionary_id(entry->path);

    /* Propagate icon detected by the parser / dict-loader fallback */
    if (dict->icon_path) {
        entry->icon_path = g_strdup(dict->icon_path);
    }

    return entry;
}

static void queue_loader_idle(LoadIdleKind kind,
                              gint generation,
                              guint completed,
                              guint total,
                              const char *status_text,
                              DictEntry *entry,
                              gboolean sync_settings) {
    LoadIdleData *ld = g_new0(LoadIdleData, 1);
    ld->kind = kind;
    ld->generation = generation;
    ld->completed = completed;
    ld->total = total;
    ld->status_text = status_text ? g_strdup(status_text) : NULL;
    ld->entry = entry;
    ld->sync_settings = sync_settings;
    g_idle_add_full(kind == LOAD_IDLE_DONE ? G_PRIORITY_LOW : G_PRIORITY_DEFAULT_IDLE,
                    on_dict_loaded_idle,
                    ld,
                    NULL);
}



static char *build_dict_metadata_text(DictEntry *entry) {
    if (!entry) {
        return NULL;
    }

    GString *metadata = g_string_new("");
    if (entry->dict && entry->dict->name && *entry->dict->name) {
        g_string_append(metadata, entry->dict->name);
    }
    if (entry->name && *entry->name && (!entry->dict || g_strcmp0(entry->name, entry->dict->name) != 0)) {
        if (metadata->len > 0) {
            g_string_append_c(metadata, ' ');
        }
        g_string_append(metadata, entry->name);
    }

    if (metadata->len == 0) {
        g_string_free(metadata, TRUE);
        return NULL;
    }

    return g_string_free(metadata, FALSE);
}

static gboolean on_dict_loaded_idle(gpointer user_data) {
    LoadIdleData *ld = user_data;

    if (ld->generation != g_atomic_int_get(&loader_generation)) {
        if (ld->kind == LOAD_IDLE_DONE) {
            /* Even if generation changed (due to cancellation), we MUST finalize loading 
             * to close the splash and restore UI state. */
            finalize_dictionary_loading(TRUE, ld->sync_settings);
        }
        if (ld->entry) {
            dict_entry_unref(ld->entry);
        }
        g_free(ld->status_text);
        g_free(ld);
        return G_SOURCE_REMOVE;
    }

    startup_splash_update_progress(ld->completed, ld->total, ld->status_text);

    if (ld->kind == LOAD_IDLE_ENTRY && ld->entry) {
        DictEntry *e = ld->entry;
        e->next = NULL;

        // Inform settings dialog(s) of finished entry
        extern void settings_scan_notify(const char *name, const char *path, int event_type);
        /* settings_scan_notify(e->name ? e->name : "(Unknown)", e->path ? e->path : "", DICT_LOADER_EVENT_FINISHED); */ // Consolidate into one place later
        if (app_settings && !settings_dictionary_enabled_by_path(app_settings, e->path, TRUE)) {
            dict_entry_unref(e);
            g_free(ld->status_text);
            g_free(ld);
            return G_SOURCE_REMOVE;
        }

        /* Check for duplicate in global list (might exist if reload/re-scan happened) */
        g_mutex_lock(&dict_loader_mutex);
        DictEntry *prev = NULL;
        DictEntry *existing = NULL;
        for (DictEntry *curr = all_dicts; curr; curr = curr->next) {
            if (curr->path && paths_are_equivalent(curr->path, e->path)) {
                existing = curr;
                break;
            }
            prev = curr;
        }
        if (existing) dict_entry_ref(existing);
        g_mutex_unlock(&dict_loader_mutex);

        if (e->dict) {
            char *guessed = NULL;
            char *metadata_text = build_dict_metadata_text(e);
            char *source_lang = e->dict->source_lang ? g_strdup(e->dict->source_lang) : NULL;
            char *target_lang = e->dict->target_lang ? g_strdup(e->dict->target_lang) : NULL;

            langpair_fill_missing(&source_lang, &target_lang, metadata_text, e->path);
            guessed = langpair_build_group_name(source_lang, target_lang);

            if (guessed && *guessed) {
                e->guessed_lang_group = g_strdup(guessed);
                if (existing) {
                    g_free(existing->guessed_lang_group);
                    existing->guessed_lang_group = g_strdup(guessed);
                }
                if (app_settings) {
                    if (e->dict_id &&
                        settings_upsert_guessed_group(app_settings, guessed, e->dict_id)) {
                        // populate_groups_sidebar(); // Removed from here to coalesce
                    }
                }
            }
            g_free(guessed);
            g_free(source_lang);
            g_free(target_lang);
            g_free(metadata_text);
            
            // Re-populate dict sidebar as well to show new group info if we decide to add it to subtitles
            // populate_dict_sidebar(); // Removed from here to coalesce
        }

        // Replace in global list under lock
        g_mutex_lock(&dict_loader_mutex);
        prev = NULL;
        DictEntry *found_again = NULL;
        for (DictEntry *curr = all_dicts; curr; curr = curr->next) {
            if (curr->path && paths_are_equivalent(curr->path, e->path)) {
                found_again = curr;
                break;
            }
            prev = curr;
        }

        if (found_again) {
            // Replace 'found_again' (which should be 'existing') with 'e'
            e->next = found_again->next;
            if (prev) {
                prev->next = e;
            } else {
                all_dicts = e;
            }
            if (active_entry == found_again) {
                set_active_entry(e);
            }
            found_again->next = NULL;
            dict_entry_unref(found_again);
        } else {
            // New unique entry - append to list
            e->next = NULL;
            if (!all_dicts) {
                all_dicts = e;
            } else {
                DictEntry *last = all_dicts;
                while (last->next) last = last->next;
                last->next = e;
            }
        }
        g_mutex_unlock(&dict_loader_mutex);
        if (existing) dict_entry_unref(existing);

        if (!active_entry && all_dicts) {
            set_active_entry(all_dicts);
        }

        /* Throttled UI update: only rebuild sidebar every 50 files to avoid UI hammering */
        if (!startup_splash_is_active() && (ld->completed % 50 == 0)) {
            populate_dict_sidebar();
        }

        // maybe_show_startup_random_word(); // Removed from here to coalesce

        /* Notify active scan dialogs that this dictionary is finished loading */
        extern void settings_scan_notify(const char *name, const char *path, int event_type);
        settings_scan_notify(e->name, e->path, DICT_LOADER_EVENT_FINISHED);
    }

    if (ld->kind == LOAD_IDLE_DONE) {
        finalize_dictionary_loading(TRUE, ld->sync_settings);
    }

    g_free(ld->status_text);
    g_free(ld);
    return G_SOURCE_REMOVE;
}

/* ── Phase 5: Parallel loading helpers (file-scope to avoid executable stack) ── */
typedef struct {
    const char *path;
    gint generation;
    gboolean discover_from_dirs;
    guint total;
    volatile gint *completed;
} LoadOneArgs;

static void load_one_dict_worker(gpointer data, gpointer user_data) {
    (void)user_data;
    LoadOneArgs *la = data;
    if (la->generation != g_atomic_int_get(&loader_generation)) {
        g_free(la);
        return;
    }

    DictFormat fmt = dict_detect_format(la->path);

    /* Notify scan dialog that this specific file is now being loaded */
    {
        extern void settings_scan_notify(const char *name, const char *path, int event_type);
        char *bn = g_path_get_basename(la->path);
        settings_scan_notify(bn ? bn : "(loading)", la->path, DICT_LOADER_EVENT_STARTED);
        g_free(bn);
    }

    fprintf(stderr, "[LOADER] Starting %s (fmt=%d)\n", la->path, fmt);
    DictMmap *dict = dict_load_any(la->path, fmt, &loader_generation, la->generation);
    fprintf(stderr, "[LOADER] Finished %s -> %s\n", la->path, dict ? "SUCCESS" : "FAILED");

    gint done = g_atomic_int_add(la->completed, 1) + 1;

    if (dict) {
        DictEntry *entry = create_dict_entry_from_loaded(la->path, fmt, dict);
        char *basename = g_path_get_basename(la->path);
        char *status = g_strdup_printf("Loading %s...", basename ? basename : "dictionary");
        queue_loader_idle(LOAD_IDLE_ENTRY, la->generation, (guint)done, la->total, status, entry,
                          la->discover_from_dirs);
        g_free(status);
        g_free(basename);
    } else {
        extern void settings_scan_notify(const char *name, const char *path, int event_type);
        char *basename = g_path_get_basename(la->path);
        settings_scan_notify(basename ? basename : "(Unknown)", la->path, DICT_LOADER_EVENT_FAILED);
        g_free(basename);
    }

    g_free(la);
}

static gpointer dict_load_thread(gpointer user_data) {
    LoadThreadArgs *args = user_data;
    GPtrArray *candidate_paths = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_mutex_lock(&loader_cancel_mutex);
    if (loader_cancellable) {
        g_object_unref(loader_cancellable);
    }
    loader_cancellable = g_cancellable_new();
    GCancellable *cancellable = g_object_ref(loader_cancellable);
    g_mutex_unlock(&loader_cancel_mutex);

    if (args->generation != g_atomic_int_get(&loader_generation)) {
        g_object_unref(cancellable);
        goto cleanup;
    }

    for (int i = 0; i < args->n_manual; i++) {
        const char *path = args->manual_paths[i];
        loader_add_candidate_path(path, candidate_paths, seen_paths, seen_dsl_families,
                                  args->ignored_paths);
    }

    gboolean has_find = g_file_test("/usr/bin/find", G_FILE_TEST_EXISTS);
    for (int i = 0; i < args->n_dirs; i++) {
        if (args->generation != g_atomic_int_get(&loader_generation)) break;
        if (g_cancellable_is_cancelled(cancellable)) break;
        if (has_find) {
            collect_dictionary_candidate_paths_with_find(args->dirs[i], candidate_paths,
                                                        seen_paths, seen_dsl_families,
                                                        args->ignored_paths, args->generation, cancellable);
        } else {
            collect_dictionary_candidate_paths_recursive(args->dirs[i], candidate_paths,
                                                        seen_paths, seen_dsl_families,
                                                        args->ignored_paths, args->generation, 0);
        }
    }
    
    g_object_unref(cancellable);

    /* Discovery happens and notifies incrementally in collectors now */

    guint total_candidates = candidate_paths->len;
    queue_loader_idle(LOAD_IDLE_STATUS, args->generation, 0, total_candidates,
                      total_candidates > 0 ? "Preparing Diction..." : "Preparing dictionary library...",
                      NULL, args->discover_from_dirs);

    /* ── Phase 5: Parallel dictionary loading ── */
    if (total_candidates > 0) {
        volatile gint completed_count = 0;

        /* Ensure strictly sequential loading to maintain UI responsiveness and reduce I/O pressure */
        guint n_workers = 1;
        GError *pool_error = NULL;
        GThreadPool *pool = g_thread_pool_new(load_one_dict_worker, NULL, (gint)n_workers, FALSE, &pool_error);

        if (pool) {
            for (guint i = 0; i < candidate_paths->len && i < (guint)MAX_DICTS; i++) {
                if (args->generation != g_atomic_int_get(&loader_generation)) break;

                LoadOneArgs *la = g_new0(LoadOneArgs, 1);
                la->path = g_ptr_array_index(candidate_paths, i);
                la->generation = args->generation;
                la->discover_from_dirs = args->discover_from_dirs;
                la->total = total_candidates;
                la->completed = &completed_count;

                g_thread_pool_push(pool, la, NULL);
            }

            /* Wait for all tasks to finish */
            g_thread_pool_free(pool, FALSE, TRUE);
        } else {
            /* Fallback to serial loading if pool creation fails */
            if (pool_error) {
                fprintf(stderr, "[LOADER] Thread pool creation failed: %s, falling back to serial\n",
                        pool_error->message);
                g_error_free(pool_error);
            }
            for (guint i = 0; i < candidate_paths->len && i < (guint)MAX_DICTS; i++) {
                if (args->generation != g_atomic_int_get(&loader_generation)) break;

                const char *path = g_ptr_array_index(candidate_paths, i);
                DictFormat fmt = dict_detect_format(path);
                DictMmap *dict = dict_load_any(path, fmt, &loader_generation, args->generation);
                if (dict) {
                    DictEntry *entry = create_dict_entry_from_loaded(path, fmt, dict);
                    char *basename = g_path_get_basename(path);
                    char *status = g_strdup_printf("Loading %s...", basename ? basename : "dictionary");
                    queue_loader_idle(LOAD_IDLE_ENTRY, args->generation, i + 1, total_candidates, status, entry,
                                      args->discover_from_dirs);
                    g_free(status);
                    g_free(basename);
                } else {
                    extern void settings_scan_notify(const char *name, const char *path, int event_type);
                    char *basename = g_path_get_basename(path);
                    settings_scan_notify(basename ? basename : "(Unknown)", path, DICT_LOADER_EVENT_FAILED);
                    g_free(basename);
                }
            }
        }
    }

cleanup:
    queue_loader_idle(LOAD_IDLE_DONE, args->generation,
                      candidate_paths ? candidate_paths->len : 0,
                      candidate_paths ? candidate_paths->len : 0,
                      "Opening Diction...", NULL, args->discover_from_dirs);

    // Free args
    if (candidate_paths) {
        g_ptr_array_free(candidate_paths, TRUE);
    }
    if (seen_dsl_families) {
        g_hash_table_unref(seen_dsl_families);
    }
    if (seen_paths) {
        g_hash_table_unref(seen_paths);
    }
    for (int i = 0; i < args->n_dirs; i++)
        g_free(args->dirs[i]);
    g_free(args->dirs);
    for (int i = 0; i < args->n_manual; i++)
        g_free(args->manual_paths[i]);
    g_free(args->manual_paths);
    if (args->ignored_paths) {
        g_hash_table_unref(args->ignored_paths);
    }
    g_free(args);
    return NULL;
}

static gboolean start_async_dict_loading(gboolean discover_from_dirs) {
    if (!app_settings)
        return FALSE;

    LoadThreadArgs *args = g_new0(LoadThreadArgs, 1);
    args->discover_from_dirs = discover_from_dirs;
    args->n_dirs = discover_from_dirs ? (int)app_settings->dictionary_dirs->len : 0;
    args->dirs   = g_new(char *, args->n_dirs + 1);
    for (int i = 0; i < args->n_dirs; i++)
        args->dirs[i] = g_strdup(g_ptr_array_index(app_settings->dictionary_dirs, i));
    args->dirs[args->n_dirs] = NULL;

    GPtrArray *manual_paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < app_settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, i);
        if (!cfg || !cfg->enabled || !cfg->path || !*cfg->path) {
            continue;
        }

        if (!discover_from_dirs ||
            g_strcmp0(cfg->source, "manual") == 0 ||
            g_strcmp0(cfg->source, "imported") == 0) {
            g_ptr_array_add(manual_paths, g_strdup(cfg->path));
        }
    }
    args->n_manual = (int)manual_paths->len;
    args->manual_paths = g_new0(char *, args->n_manual + 1);
    for (int i = 0; i < args->n_manual; i++) {
        args->manual_paths[i] = g_ptr_array_index(manual_paths, i);
    }
    g_ptr_array_free(manual_paths, FALSE);

    args->ignored_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < app_settings->ignored_dictionary_paths->len; i++) {
        const char *ignored_path = g_ptr_array_index(app_settings->ignored_dictionary_paths, i);
        if (ignored_path && *ignored_path) {
            g_hash_table_add(args->ignored_paths, g_strdup(ignored_path));
        }
    }

    if (args->n_dirs == 0 && args->n_manual == 0) {
        g_free(args->dirs);
        g_free(args->manual_paths);
        if (args->ignored_paths) {
            g_hash_table_unref(args->ignored_paths);
        }
        g_free(args);
        return FALSE;
    }

    args->generation = g_atomic_int_get(&loader_generation);
    dictionary_loading_in_progress = TRUE;
    GThread *thread = g_thread_new("dict-loader", dict_load_thread, args);
    g_thread_unref(thread); // fire-and-forget
    return TRUE;
}

static void on_toggle_sidebar(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    AdwOverlaySplitView *split_view = ADW_OVERLAY_SPLIT_VIEW(user_data);
    adw_overlay_split_view_set_show_sidebar(split_view, !adw_overlay_split_view_get_show_sidebar(split_view));
}

/* Removed update_content_menu_button_visibility */
static void on_search_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    reveal_search_entry(FALSE);
}

static void on_search_entry_focus_leave(GtkEventControllerFocus *controller, gpointer user_data) {
    (void)controller; (void)user_data;
    if (search_stack) {
        gtk_stack_set_visible_child_name(search_stack, "button");
    }
}

static gboolean on_search_btn_drop(GtkDropTarget *target, const GValue *value, gdouble x, gdouble y, gpointer data) {
    (void)target; (void)x; (void)y; (void)data;
    if (G_VALUE_HOLDS_STRING(value)) {
        const char *text = g_value_get_string(value);
        if (text && *text && search_entry && search_stack) {
            gtk_editable_set_text(GTK_EDITABLE(search_entry), text);
            reveal_search_entry(FALSE);
            return TRUE;
        }
    }
    return FALSE;
}



static gboolean is_small_scan_mode = FALSE;
static GtkWidget *zoom_to_restore_btn = NULL;

static void app_show_window(void) {
    if (!main_window) return;
    is_small_scan_mode = FALSE;
    if (zoom_to_restore_btn) {
        gtk_widget_set_visible(zoom_to_restore_btn, FALSE);
    }
    gtk_window_set_default_size(GTK_WINDOW(main_window), 1000, 650);
    gtk_window_present(main_window);
}

static void on_zoom_to_restore_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    app_show_window();
}

static char *pending_activation_token = NULL;

static void on_global_shortcut_activated(const char *activation_token, gpointer user_data) {
    (void)user_data;
    g_free(pending_activation_token);
    pending_activation_token = activation_token ? g_strdup(activation_token) : NULL;
    scan_popup_trigger_manual();
}

static void scan_word_callback(const char *word) {
    if (!word || !*word || !main_window || !search_entry) return;
    
    is_small_scan_mode = TRUE;
    
    if (gtk_window_is_maximized(main_window)) {
        gtk_window_unmaximize(main_window);
    }
    
    gtk_window_set_default_size(GTK_WINDOW(main_window), 400, 500);
    
    if (zoom_to_restore_btn) {
        gtk_widget_set_visible(zoom_to_restore_btn, TRUE);
    }
    
    if (pending_activation_token) {
        gtk_window_set_startup_id(GTK_WINDOW(main_window), pending_activation_token);
        g_free(pending_activation_token);
        pending_activation_token = NULL;
    }
    gtk_window_present(main_window);

    gtk_editable_set_text(GTK_EDITABLE(search_entry), word);
    reveal_search_entry(FALSE);
}

static gboolean on_window_close_request(GtkWindow *window, gpointer user_data) {
    (void)user_data;
    if (app_settings && app_settings->close_to_tray && app_settings->tray_icon_enabled) {
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_new_tab_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    AdwTabPage *page = create_new_tab("Search", TRUE);
    (void)page;
    reveal_search_entry(FALSE);
}

static gboolean on_tab_close(AdwTabView *view, AdwTabPage *page, gpointer user_data) {
    (void)user_data;
    adw_tab_view_close_page_finish(view, page, TRUE);
    if (adw_tab_view_get_n_pages(view) == 0) {
        create_new_tab("Home", TRUE);
    }
    update_nav_buttons_state();
    return TRUE; /* Handled */
}

static AdwTabPage *create_new_tab(const char *title, gboolean select_it) {
    if (!font_ucm) {
        font_ucm = webkit_user_content_manager_new();
        apply_font_to_webview(NULL);
    }
    WebKitWebView *wv = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "user-content-manager", font_ucm, NULL));
    
    WebKitSettings *web_settings = webkit_web_view_get_settings(wv);
    webkit_settings_set_auto_load_images(web_settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(web_settings, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(web_settings, TRUE);

    if (app_settings) {
        if (app_settings->font_family && *app_settings->font_family)
            webkit_settings_set_default_font_family(web_settings, app_settings->font_family);
        if (app_settings->font_size > 0)
            webkit_settings_set_default_font_size(web_settings, (guint32)app_settings->font_size);
    }
    
    if (app_settings) {
        dsl_theme_palette palette;
        gboolean is_dark = style_manager && adw_style_manager_get_dark(style_manager);
        dict_render_get_theme_palette(app_settings->color_theme, is_dark, &palette);

        GdkRGBA bg_color;
        if (!gdk_rgba_parse(&bg_color, palette.bg)) {
            gdk_rgba_parse(&bg_color, is_dark ? "#1e1e21" : "#ffffff");
        }
        webkit_web_view_set_background_color(wv, &bg_color);
    }

    g_signal_connect(wv, "decide-policy", G_CALLBACK(on_decide_policy), search_entry);
    g_signal_connect(wv, "load-changed", G_CALLBACK(on_web_view_load_changed), NULL);
    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
    g_signal_connect(fc, "counted-matches", G_CALLBACK(on_find_counted_matches), NULL);

    GtkWidget *web_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(web_scroll, TRUE);
    gtk_widget_set_hexpand(web_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(web_scroll), GTK_WIDGET(wv));
    g_object_set_data(G_OBJECT(web_scroll), "web-view", wv);
    
    AdwTabPage *page = adw_tab_view_append(tab_view, web_scroll);
    adw_tab_page_set_title(page, title ? title : "Search");
    set_tab_full_text_search(page, FALSE);
    
    if (select_it) {
        adw_tab_view_set_selected_page(tab_view, page);
    }
    
    return page;
}

static void on_tab_selected(AdwTabView *view, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    AdwTabPage *page = adw_tab_view_get_selected_page(view);
    sync_full_text_search_action_state();
    if (page && search_entry) {
        const char *query = (const char *)g_object_get_data(G_OBJECT(page), "search-query");
        gboolean is_fts = tab_page_is_full_text_search(page);
        
        g_signal_handlers_block_by_func(search_entry, on_search_changed, NULL);
        if (query) {
            char *display_query = normalize_headword_for_render(query, strlen(query), FALSE);
            gtk_editable_set_text(GTK_EDITABLE(search_entry), display_query);
            if (search_button_label) gtk_label_set_text(GTK_LABEL(search_button_label), display_query);
            g_free(display_query);
        } else {
            gtk_editable_set_text(GTK_EDITABLE(search_entry), "");
            if (search_button_label) gtk_label_set_text(GTK_LABEL(search_button_label), "Search");
        }
        g_signal_handlers_unblock_by_func(search_entry, on_search_changed, NULL);

        if (is_fts) {
            char *clean_query = normalize_headword_for_search(query, FALSE);
            if (clean_query && *clean_query) {
                populate_search_sidebar_with_mode(query, TRUE);
            } else {
                cancel_sidebar_search();
                g_clear_pointer(&fts_highlight_query, g_free);
                populate_search_sidebar_status("Full Text Search", "Type a word or phrase to search definitions in this scope.");
            }
            g_free(clean_query);
        } else {
            populate_search_sidebar_with_mode(query, FALSE);
        }
        populate_dict_sidebar();
    }
}

static void on_scope_button_active_changed(GtkMenuButton *btn, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    /* Only act on close (active → FALSE); while opening the popover owns focus. */
    if (!gtk_menu_button_get_active(btn)) {
        GtkWidget *entry = GTK_WIDGET(user_data);
        if (entry && gtk_widget_get_mapped(entry))
            gtk_widget_grab_focus(entry);
    }
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    if (main_window) {
        app_show_window();
        return;
    }
    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    main_window = GTK_WINDOW(window);

    style_manager = adw_style_manager_get_default();
    update_theme_colors();

    g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&main_window);
    gtk_window_set_title(GTK_WINDOW(window), "Diction");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 650);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), NULL);

    if (app_settings && app_settings->tray_icon_enabled) {
        tray_icon_init(app, main_window, app_show_window, toggle_scan_from_tray, quit_from_tray);
        tray_icon_set_scan_active(app_settings->scan_popup_enabled);
    }
    scan_popup_init(app, app_settings, scan_word_callback);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), main_box);

    AdwOverlaySplitView *split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_max_sidebar_width(split_view, 360.0);
    gtk_widget_set_vexpand(GTK_WIDGET(split_view), TRUE);
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(split_view));
    /* Auto-hide sidebar below 720px width */
    AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 720px"));
    GValue collapsed_val = G_VALUE_INIT;
    g_value_init(&collapsed_val, G_TYPE_BOOLEAN);
    g_value_set_boolean(&collapsed_val, TRUE);
    adw_breakpoint_add_setter(breakpoint, G_OBJECT(split_view), "collapsed", &collapsed_val);
        g_value_unset(&collapsed_val);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), breakpoint);
    /* --- Sidebar --- */
    GtkWidget *sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar_vbox, "sidebar");

    /* Sidebar Header */
    GtkWidget *sidebar_header = adw_header_bar_new();
    gtk_widget_add_css_class(sidebar_header, "sidebar");
    gtk_widget_add_css_class(sidebar_header, "flat");
    GtkWidget *title_label = gtk_label_new("Diction");
    gtk_widget_add_css_class(title_label, "title");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(sidebar_header), title_label);

    GtkWidget *random_btn = gtk_button_new_from_icon_name("dice3-symbolic");
    gtk_widget_add_css_class(random_btn, "flat");
    gtk_widget_set_tooltip_text(random_btn, "Random Headword");
    g_signal_connect(random_btn, "clicked", G_CALLBACK(on_random_clicked), NULL);
    adw_header_bar_pack_start(ADW_HEADER_BAR(sidebar_header), random_btn);

    GtkWidget *new_tab_btn = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_widget_add_css_class(new_tab_btn, "flat");
    gtk_widget_set_tooltip_text(new_tab_btn, "New Search Tab");
    g_signal_connect(new_tab_btn, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(sidebar_header), new_tab_btn);

    gtk_box_append(GTK_BOX(sidebar_vbox), sidebar_header);

    /* Sidebar Stack */
    AdwViewStack *sidebar_stack = ADW_VIEW_STACK(adw_view_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(sidebar_stack), TRUE);

    /* Search/Related Tab */
    GtkWidget *related_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(related_scroll, TRUE);
    gtk_widget_set_hexpand(related_scroll, TRUE);
    related_string_list = gtk_string_list_new(NULL);
    related_row_payloads = g_ptr_array_new_with_free_func((GDestroyNotify)related_row_payload_free);
    related_selection_model = GTK_SINGLE_SELECTION(gtk_single_selection_new(G_LIST_MODEL(related_string_list)));
    gtk_single_selection_set_autoselect(related_selection_model, FALSE);
    gtk_single_selection_set_can_unselect(related_selection_model, FALSE);

    GtkListItemFactory *related_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(related_factory, "setup", G_CALLBACK(related_list_item_setup), NULL);
    g_signal_connect(related_factory, "bind", G_CALLBACK(related_list_item_bind), NULL);
    g_signal_connect(related_factory, "unbind", G_CALLBACK(related_list_item_unbind), NULL);

    related_list_view = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(related_selection_model), related_factory));
    gtk_list_view_set_single_click_activate(related_list_view, TRUE);
    g_signal_connect(related_list_view, "activate", G_CALLBACK(on_related_item_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(related_scroll), GTK_WIDGET(related_list_view));
    adw_view_stack_add_titled_with_icon(sidebar_stack, related_scroll, "search", "Search", "system-search-symbolic");

    /* Dictionaries Tab */
    GtkWidget *dict_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(dict_scroll, TRUE);
    gtk_widget_set_hexpand(dict_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dict_scroll),
        create_sidebar_list_view(&dict_sidebar, G_CALLBACK(on_dict_item_activated)));
    adw_view_stack_add_titled_with_icon(sidebar_stack, dict_scroll, "dictionaries", "Dictionaries", "accessories-dictionary-symbolic");

    /* History Tab */
    GtkWidget *history_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(history_scroll, TRUE);
    gtk_widget_set_hexpand(history_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(history_scroll),
        create_sidebar_list_view(&history_sidebar, G_CALLBACK(on_history_item_activated)));
    adw_view_stack_add_titled_with_icon(sidebar_stack, history_scroll, "history", "History", "document-open-recent-symbolic");

    /* Favorites Tab */
    GtkWidget *favorites_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(favorites_scroll, TRUE);
    gtk_widget_set_hexpand(favorites_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(favorites_scroll),
        create_sidebar_list_view(&favorites_sidebar, G_CALLBACK(on_favorites_item_activated)));
    adw_view_stack_add_titled_with_icon(sidebar_stack, favorites_scroll, "favorites", "Favorites", "starred-symbolic");



    gtk_box_append(GTK_BOX(sidebar_vbox), GTK_WIDGET(sidebar_stack));

    /* Custom Bottom Tabs (Python style) */
    GtkWidget *tabs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(tabs_box, 2);
    gtk_widget_set_margin_bottom(tabs_box, 3);
    gtk_widget_set_margin_start(tabs_box, 2);
    gtk_widget_set_margin_end(tabs_box, 2);
    gtk_widget_set_halign(tabs_box, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(tabs_box, "linked");

    const char *tabs[][3] = {
        {"system-search-symbolic", "search", "Search"},
        {"starred-symbolic", "favorites", "Favorites"},
        {"document-open-recent-symbolic", "history", "History"},
        {"accessories-dictionary-symbolic", "dictionaries", "Dictionaries"}
    };

    GtkWidget *first_btn = NULL;
    for (int i = 0; i < 4; i++) {
        GtkWidget *btn = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(btn), tabs[i][0]);
        gtk_widget_set_tooltip_text(btn, tabs[i][2]);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_set_hexpand(btn, TRUE);
        
        if (i == 0) {
            first_btn = btn;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);
        } else {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), GTK_TOGGLE_BUTTON(first_btn));
        }

        g_object_set_data_full(G_OBJECT(btn), "stack-name", g_strdup(tabs[i][1]), g_free);
        g_object_set_data(G_OBJECT(btn), "stack-widget", sidebar_stack);
        
        g_signal_connect(btn, "toggled", G_CALLBACK(on_sidebar_tab_toggled), NULL);

        gtk_box_append(GTK_BOX(tabs_box), btn);
    }
    gtk_widget_add_css_class(tabs_box, "sidebar-tabs");
    gtk_widget_add_css_class(tabs_box, "sidebar");
    gtk_widget_add_css_class(tabs_box, "flat");
    gtk_box_append(GTK_BOX(sidebar_vbox), tabs_box);

    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_view), sidebar_vbox);

    /* --- Content --- */
    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    
    GtkWidget *content_header = adw_header_bar_new();
    gtk_widget_add_css_class(content_header, "content-header");
    adw_toolbar_view_add_top_bar(toolbar_view, content_header);

    /* Sidebar toggle action */
    GSimpleAction *toggle_sidebar_action = g_simple_action_new("toggle-sidebar", NULL);
    g_signal_connect(toggle_sidebar_action, "activate", G_CALLBACK(on_toggle_sidebar), split_view);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(toggle_sidebar_action));

    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(search_box, "linked");
    gtk_widget_set_hexpand(search_box, TRUE);

    nav_back_btn = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_add_css_class(nav_back_btn, "flat");
    g_signal_connect(nav_back_btn, "clicked", G_CALLBACK(on_nav_back_clicked), NULL);
    gtk_widget_set_sensitive(nav_back_btn, FALSE);

    nav_forward_btn = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(nav_forward_btn, "flat");
    g_signal_connect(nav_forward_btn, "clicked", G_CALLBACK(on_nav_forward_clicked), NULL);
    gtk_widget_set_sensitive(nav_forward_btn, FALSE);

    /* Use GtkEntry (not GtkSearchEntry) so we can set the primary icon freely
     * via gtk_entry_set_icon_from_icon_name — GtkSearchEntry in GTK4 owns its
     * icon slot and overrides any external set. */
    search_entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(search_entry), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(search_entry), "search-entry-joined");
    gtk_entry_set_placeholder_text(search_entry, "Search");
    gtk_entry_set_icon_from_icon_name(search_entry, GTK_ENTRY_ICON_PRIMARY, "system-search-symbolic");
    /* 'changed' fires on every keystroke; our schedule_execute_search debounces it. */
    g_signal_connect(search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    /* Activate (Enter key) triggers an immediate search */
    g_signal_connect(search_entry, "activate", G_CALLBACK(execute_search_now), NULL);

    GtkEventController *focus_ctrl = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl, "leave", G_CALLBACK(on_search_entry_focus_leave), NULL);
    gtk_widget_add_controller(GTK_WIDGET(search_entry), focus_ctrl);

    GtkWidget *btn_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_content, GTK_ALIGN_FILL);
    search_mode_icon = GTK_IMAGE(gtk_image_new_from_icon_name("system-search-symbolic"));
    gtk_widget_set_opacity(GTK_WIDGET(search_mode_icon), 0.7); // make icon slightly dim
    search_button_label = GTK_LABEL(gtk_label_new("Search"));
    gtk_widget_set_opacity(GTK_WIDGET(search_button_label), 0.7); // make label text dim
    gtk_label_set_ellipsize(search_button_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(search_button_label, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(search_button_label), TRUE);
    // Move label to the left
    gtk_widget_set_halign(GTK_WIDGET(search_button_label), GTK_ALIGN_START);
    
    gtk_box_append(GTK_BOX(btn_content), GTK_WIDGET(search_mode_icon));
    gtk_box_append(GTK_BOX(btn_content), GTK_WIDGET(search_button_label));

    search_button = GTK_BUTTON(gtk_button_new());
    gtk_button_set_child(search_button, btn_content);
    gtk_widget_add_css_class(GTK_WIDGET(search_button), "search-button-bg");
    gtk_widget_set_hexpand(GTK_WIDGET(search_button), TRUE);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_button_clicked), NULL);

    GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_search_btn_drop), NULL);
    gtk_widget_add_controller(GTK_WIDGET(search_button), GTK_EVENT_CONTROLLER(drop_target));

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(button_box, TRUE);
    gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(search_button));

    search_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(search_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand(GTK_WIDGET(search_stack), TRUE);
    update_search_mode_visuals(FALSE);
    
    gtk_stack_add_named(search_stack, button_box, "button");
    gtk_stack_add_named(search_stack, GTK_WIDGET(search_entry), "entry");
    gtk_stack_set_visible_child_name(search_stack, "button");

    GtkWidget *scope_btn_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *scope_icon = gtk_image_new_from_icon_name("dictionary-symbolic");
    search_scope_button_label = GTK_LABEL(gtk_label_new("All"));
    gtk_label_set_ellipsize(search_scope_button_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(search_scope_button_label, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(search_scope_button_label), FALSE);
    GtkWidget *scope_arrow = gtk_image_new_from_icon_name("pan-down-symbolic");
    gtk_box_append(GTK_BOX(scope_btn_content), scope_icon);
    gtk_box_append(GTK_BOX(scope_btn_content), GTK_WIDGET(search_scope_button_label));
    gtk_box_append(GTK_BOX(scope_btn_content), scope_arrow);

    search_scope_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_widget_add_css_class(GTK_WIDGET(search_scope_button), "flat");
    gtk_widget_add_css_class(GTK_WIDGET(search_scope_button), "search-scope-button");
    gtk_menu_button_set_child(search_scope_button, scope_btn_content);
    gtk_widget_set_tooltip_text(GTK_WIDGET(search_scope_button), "Search scope");
    /* After the scope popover closes, GTK leaves focus on the GtkMenuButton which
     * causes a persistent highlight that CSS :focus rules cannot reliably clear.
     * on_scope_button_active_changed() moves focus to the search entry so the
     * button returns to its resting state. */
    g_signal_connect(search_scope_button, "notify::active",
        G_CALLBACK(on_scope_button_active_changed), search_entry);

    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), nav_back_btn);
    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), nav_forward_btn);

    gtk_box_append(GTK_BOX(search_box), GTK_WIDGET(search_stack));
    gtk_box_append(GTK_BOX(search_box), GTK_WIDGET(search_scope_button));

    adw_header_bar_set_title_widget(ADW_HEADER_BAR(content_header), search_box);

    zoom_to_restore_btn = gtk_button_new_from_icon_name("window-maximize-symbolic");
    gtk_widget_add_css_class(zoom_to_restore_btn, "flat");
    gtk_widget_set_tooltip_text(zoom_to_restore_btn, "Restore normal size");
    g_signal_connect(zoom_to_restore_btn, "clicked", G_CALLBACK(on_zoom_to_restore_clicked), NULL);
    gtk_widget_set_visible(zoom_to_restore_btn, FALSE);
    adw_header_bar_pack_end(ADW_HEADER_BAR(content_header), zoom_to_restore_btn);

    GMenu *menu = g_menu_new();
    GMenu *search_menu = g_menu_new();
    g_menu_append(search_menu, "Full Text Search", "app.full-text-search");
    g_menu_append_submenu(menu, "Search", G_MENU_MODEL(search_menu));
    g_object_unref(search_menu);
    g_menu_append(menu, "Preferences", "app.settings");
    GMenu *view_menu = g_menu_new();
    g_menu_append(view_menu, "Show/Hide Sidebar", "app.toggle-sidebar");
    g_menu_append(view_menu, "Zoom In", "app.zoom-in");
    g_menu_append(view_menu, "Zoom Out", "app.zoom-out");
    g_menu_append(view_menu, "Reset Zoom", "app.zoom-reset");
    g_menu_append_submenu(menu, "View", G_MENU_MODEL(view_menu));
    g_object_unref(view_menu);
    g_menu_append(menu, "About", "app.about");

    GtkWidget *content_settings_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(content_settings_btn), "open-menu-symbolic");
    gtk_widget_add_css_class(content_settings_btn, "flat");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(content_settings_btn), G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(content_header), content_settings_btn);

    /* WebKit Tabs */
    tab_view = ADW_TAB_VIEW(adw_tab_view_new());
    AdwTabBar *tab_bar = ADW_TAB_BAR(adw_tab_bar_new());
    adw_tab_bar_set_view(tab_bar, tab_view);
    adw_tab_bar_set_autohide(tab_bar, TRUE);
    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(tab_bar));

    g_signal_connect(tab_view, "notify::selected-page", G_CALLBACK(on_tab_selected), NULL);
    g_signal_connect(tab_view, "close-page", G_CALLBACK(on_tab_close), NULL);
    
    create_new_tab("Home", TRUE);

    GtkWidget *content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(content_vbox, "article-view-container");
    gtk_box_append(GTK_BOX(content_vbox), GTK_WIDGET(tab_view));
    gtk_box_append(GTK_BOX(content_vbox), create_find_bar());

    adw_toolbar_view_set_content(toolbar_view, content_vbox);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_view), GTK_WIDGET(toolbar_view));

    GSimpleAction *find_action = g_simple_action_new("find", NULL);
    g_signal_connect(find_action, "activate", G_CALLBACK(on_find_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(find_action));

    GSimpleAction *focus_search_action = g_simple_action_new("focus-search", NULL);
    g_signal_connect(focus_search_action, "activate", G_CALLBACK(on_focus_search_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(focus_search_action));

    search_scope_action = g_simple_action_new_stateful("search-scope",
                                                       G_VARIANT_TYPE_STRING,
                                                       g_variant_new_string(active_scope_id ? active_scope_id : "all"));
    g_signal_connect(search_scope_action, "activate", G_CALLBACK(on_search_scope_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(search_scope_action));

    full_text_search_toggle_action = g_simple_action_new("full-text-search", NULL);
    g_signal_connect(full_text_search_toggle_action, "activate", G_CALLBACK(on_full_text_search_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(full_text_search_toggle_action));

    GSimpleAction *zoom_in_action = g_simple_action_new("zoom-in", NULL);
    g_signal_connect(zoom_in_action, "activate", G_CALLBACK(on_zoom_in_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(zoom_in_action));

    GSimpleAction *zoom_out_action = g_simple_action_new("zoom-out", NULL);
    g_signal_connect(zoom_out_action, "activate", G_CALLBACK(on_zoom_out_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(zoom_out_action));

    GSimpleAction *zoom_reset_action = g_simple_action_new("zoom-reset", NULL);
    g_signal_connect(zoom_reset_action, "activate", G_CALLBACK(on_zoom_reset_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(zoom_reset_action));

    GtkShortcutController *shortcut_ctrl = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    gtk_shortcut_controller_set_scope(shortcut_ctrl, GTK_SHORTCUT_SCOPE_GLOBAL);
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(shortcut_ctrl));

    gtk_shortcut_controller_add_shortcut(shortcut_ctrl, gtk_shortcut_new(
        gtk_keyval_trigger_new(GDK_KEY_f, GDK_CONTROL_MASK),
        gtk_named_action_new("app.find")));

    gtk_shortcut_controller_add_shortcut(shortcut_ctrl, gtk_shortcut_new(
        gtk_keyval_trigger_new(GDK_KEY_d, GDK_CONTROL_MASK | GDK_ALT_MASK),
        gtk_named_action_new("app.scan-clipboard")));
    
    gtk_shortcut_controller_add_shortcut(shortcut_ctrl, gtk_shortcut_new(
        gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
        gtk_callback_action_new((GtkShortcutFunc)on_find_shortcut_close, NULL, NULL)));

    const char *focus_search_accels[] = { "<Primary>l", "<Alt>d", NULL };
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.focus-search", focus_search_accels);

    const char *full_text_search_accels[] = { "<Primary><Shift>f", NULL };
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.full-text-search", full_text_search_accels);

    const char *zoom_in_accels[] = { "<Primary>equal", "<Primary>plus", "<Primary>KP_Add", NULL };
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.zoom-in", zoom_in_accels);

    const char *zoom_out_accels[] = { "<Primary>minus", "<Primary>KP_Subtract", NULL };
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.zoom-out", zoom_out_accels);

    const char *zoom_reset_accels[] = { "<Primary>0", "<Primary>KP_0", NULL };
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.zoom-reset", zoom_reset_accels);

    gboolean had_cached_entries = FALSE;
    gboolean discover_from_dirs = FALSE;
    if (!all_dicts && app_settings) {
        had_cached_entries = rebuild_dict_entries_from_settings() > 0;
        discover_from_dirs = app_settings->dictionary_dirs &&
                             app_settings->dictionary_dirs->len > 0;
    }
    refresh_dictionary_directory_monitors();

    /* Populate sidebar */
    populate_dict_sidebar();
    populate_history_sidebar();
    populate_favorites_sidebar();
    rebuild_search_scope_menu();
    update_search_scope_button_label();

    populate_search_sidebar(NULL);

    /* Auto-select first dictionary */
    if (all_dicts) {
        set_active_entry(all_dicts);
        populate_dict_sidebar();
    }

    // Initialize style manager for theme support
    style_manager = adw_style_manager_get_default();
    g_signal_connect(style_manager, "notify::dark", G_CALLBACK(on_style_manager_changed), NULL);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css_provider,
    ".sidebar-tabs { border-top: 0px solid alpha(@theme_fg_color, 0.1); }"
    ".sidebar-tabs button { padding-left: 12px; padding-right: 12px; padding-top: 8px; padding-bottom: 8px; margin-left: 0.5px; margin-right: 0.5px; min-height: 0; min-width: 0; border: none; border-radius: 10px; }"
    ".sidebar-tabs button image { opacity: 0.7; }"
    ".sidebar-tabs button:checked { background: alpha(@theme_fg_color, 0.1); }"
    ".sidebar-tabs button:checked image { opacity: 1.0; }"

    ".sidebar-row { min-height: 34px; }"
    ".content-header { background: transparent; }\n"
    ".menu-item { font-weight: normal; padding: 4px 8px; min-height: 0; }"

    "overlay-split-view > separator { background: @sidebar_bg_color; min-width: 1px; opacity: 1; }"
    "headerbar.sidebar { box-shadow: none; border-bottom: none; margin: 0; padding: 0; }"
    
    /* search entry, left side of the search bar */
    ".linked entry { background: alpha(@theme_fg_color, 0.1); border: none; border-radius: 8px 0 0 8px; }"
    ".linked button { background: alpha(@theme_fg_color, 0.1); border-radius: 0; }"
    ".linked button:last-child { border-radius: 0 8px 8px 0; }"
    ".linked button:first-child { border-radius: 8px 0 0 8px; }"
    /* search drop down button, right side of the search bar */
    ".linked .search-scope-button { background: alpha(@theme_fg_color, 0.06); border: none; border-radius: 0 8px 8px 0; }"
    ".linked .search-scope-button:hover { background: alpha(@theme_fg_color, 0.15); }"
    ".linked .search-scope-button:active { background: alpha(@theme_fg_color, 0.2); }"
    ".linked .search-scope-button button { background: none; border: none; box-shadow: none; border-radius: 0; }"
    ".linked .search-scope-button button:hover { background: none; }"
    ".linked > separator { opacity: 0; min-width: 0; }"
    ".linked entry, .linked button, .linked .search-scope-button button { padding-left: 8px; padding-right: 8px; }"
    ".linked entry:hover, .linked button:hover { background: alpha(@theme_fg_color, 0.12); }"
    ".linked .linked button:checked, .search-scope-button:checked { background: alpha(@theme_fg_color, 0.06); }"
);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Apply saved theme preference
    if (app_settings) {
        if (g_strcmp0(app_settings->theme, "light") == 0)
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
        else if (g_strcmp0(app_settings->theme, "dark") == 0)
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
        else
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_DEFAULT);
    }
    update_theme_colors();

    apply_font_to_webview(NULL);

    gboolean needs_async_load = discover_from_dirs || had_cached_entries;
    render_idle_page_to_webview(
        web_view,
        needs_async_load ? "Loading dictionaries..." : "Diction",
        needs_async_load ? "Please wait." : "Start typing to search...");

    // Start async loading for configured directories so startup scan/progress
    // remains visible. The loaders should hit valid caches instead of rebuilding.
    if (needs_async_load) {
        startup_random_word_pending = (!search_entry ||
                                       strlen(gtk_editable_get_text(GTK_EDITABLE(search_entry))) == 0);
        if (start_async_dict_loading(discover_from_dirs)) {
            startup_splash_show(app, main_window, G_CALLBACK(request_loader_cancel));
        } else {
            startup_random_word_pending = FALSE;
            finalize_dictionary_loading(TRUE, discover_from_dirs);
            gtk_window_present(GTK_WINDOW(window));
        }
    } else {
        startup_random_word_pending = FALSE;
        // CLI-mode: dicts already loaded synchronously, just populate
        populate_dict_sidebar();
        if (all_dicts) {
            set_active_entry(all_dicts);
            populate_dict_sidebar();
        }
        render_idle_page_to_webview(
            web_view,
            "Welcome to Diction",
            "Select a dictionary from the sidebar and start searching.");
        gtk_window_present(GTK_WINDOW(window));
    }

    /* Debug: auto-open preferences for integrated scanning if requested. */
    if (getenv("DICTION_DEBUG_AUTO_SCAN")) {
        show_settings_dialog(NULL, NULL, app);
    }

    global_shortcut_setup(on_global_shortcut_activated, NULL);
}



/* Cancel in-flight idle/timeout sources before GTK finalization destroys widgets.
 * The GApplication::shutdown signal fires while the main loop is still active,
 * so removing sources here prevents callbacks from hitting freed objects. */
static void on_app_shutdown(GApplication *app, gpointer user_data) {
    (void)app; (void)user_data;
    cancel_sidebar_search();
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }
    if (dictionary_watch_reload_source_id != 0) {
        g_source_remove(dictionary_watch_reload_source_id);
        dictionary_watch_reload_source_id = 0;
    }
    if (dictionary_dir_monitors) {
        g_hash_table_destroy(dictionary_dir_monitors);
        dictionary_dir_monitors = NULL;
    }
    if (dictionary_root_parent_monitors) {
        g_hash_table_destroy(dictionary_root_parent_monitors);
        dictionary_root_parent_monitors = NULL;
    }
    /* Null out global widget pointers so any stray callback that somehow
     * still runs will see NULL and bail out early. */
    related_string_list = NULL;
    related_row_payloads = NULL;
}

static const char* dict_format_to_str(DictFormat fmt) {
    switch(fmt) {
        case DICT_FORMAT_DSL: return "DSL";
        case DICT_FORMAT_STARDICT: return "StarDict";
        case DICT_FORMAT_MDX: return "MDX";
        case DICT_FORMAT_BGL: return "BGL";
        case DICT_FORMAT_SLOB: return "Slob";
        case DICT_FORMAT_XDXF: return "XDXF";
        case DICT_FORMAT_DICTD: return "Dictd";
        case DICT_FORMAT_SDICT: return "Sdict";
        default: return "Unknown";
    }
}

static int run_cli_search(const char *query, const char *in_dict) {
    if (!query) {
        g_printerr("Usage: diction <query> [--in-dict=<name_or_path>]\n");
        return 1;
    }

    g_print("CLI Search for: '%s'\n", query);
    if (in_dict) g_print("Filter: '%s'\n", in_dict);

    /* 1. Load system dictionaries first */
    if (app_settings && app_settings->dictionary_dirs) {
        for (guint i = 0; i < app_settings->dictionary_dirs->len; i++) {
            const char *dir = g_ptr_array_index(app_settings->dictionary_dirs, i);
            DictEntry *scanned = dict_loader_scan_directory(dir);
            if (scanned) {
                if (!all_dicts) all_dicts = scanned;
                else {
                    DictEntry *last = all_dicts;
                    while (last->next) last = last->next;
                    last->next = scanned;
                }
            }
        }
    }

    /* 1b. Load manually configured dictionaries */
    if (app_settings && app_settings->dictionaries) {
        for (guint i = 0; i < app_settings->dictionaries->len; i++) {
            DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, i);
            if (!cfg || !cfg->enabled || !cfg->path || !*cfg->path) continue;

            gboolean already_loaded = FALSE;
            for (DictEntry *e = all_dicts; e; e = e->next) {
                if (g_strcmp0(e->path, cfg->path) == 0) {
                    already_loaded = TRUE;
                    break;
                }
            }
            if (!already_loaded) {
                DictFormat fmt = dict_detect_format(cfg->path);
                DictMmap *d = dict_load_any(cfg->path, fmt, NULL, 0);
                if (d) {
                    DictEntry *e = g_new0(DictEntry, 1);
                    e->magic = 0xDEADC0DE;
                    e->ref_count = 1;
                    e->name = g_strdup(cfg->name ? cfg->name : "dictionary");
                    e->path = g_strdup(cfg->path);
                    e->format = fmt;
                    e->dict = d;
                    e->next = all_dicts;
                    all_dicts = e;
                }
            }
        }
    }

    /* 2. If in_dict is a path to a file, load it specifically if not present */
    if (in_dict && (g_str_has_suffix(in_dict, ".dsl") || g_str_has_suffix(in_dict, ".dz") || 
                    g_str_has_suffix(in_dict, ".mdx") || g_str_has_suffix(in_dict, ".bgl") ||
                    g_str_has_suffix(in_dict, ".slob") || g_str_has_suffix(in_dict, ".xdxf") ||
                    g_str_has_suffix(in_dict, ".xdxf.dz") || g_str_has_suffix(in_dict, ".index") ||
                    g_str_has_suffix(in_dict, ".dct"))) {
        struct stat st;
        if (stat(in_dict, &st) == 0 && S_ISREG(st.st_mode)) {
            gboolean already_loaded = FALSE;
            for (DictEntry *e = all_dicts; e; e = e->next) {
                if (g_strcmp0(e->path, in_dict) == 0) {
                    already_loaded = TRUE;
                    break;
                }
            }
            if (!already_loaded) {
                DictFormat fmt = dict_detect_format(in_dict);
                DictMmap *d = dict_load_any(in_dict, fmt, NULL, 0);
                if (d) {
                    DictEntry *e = g_new0(DictEntry, 1);
                    e->magic = 0xDEADC0DE;
                    e->ref_count = 1;
                    const char *slash = strrchr(in_dict, '/');
                    e->name = g_strdup(slash ? slash + 1 : in_dict);
                    e->path = g_strdup(in_dict);
                    e->format = fmt;
                    e->dict = d;
                    e->next = all_dicts;
                    all_dicts = e;
                    g_print("Loaded dictionary: %s\n", in_dict);
                }
            }
        }
    }

    /* 3. Execute search */
    const char *normalized_query = query;
    int total_found = 0;

    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (!e->dict) continue;

        /* Filter by name or path if in_dict is set */
        if (in_dict) {
            if (g_strcmp0(e->name, in_dict) != 0 && g_strcmp0(e->path, in_dict) != 0) {
                continue;
            }
        }

        g_print("Searching in: %s... ", e->name);
        size_t pos = flat_index_search(e->dict->index, normalized_query);
        g_print("Result: %zd\n", pos);
        gboolean dict_header_printed = FALSE;

        while (pos != (size_t)-1) {
            const FlatTreeEntry *res = flat_index_get(e->dict->index, pos);
            if (!res) break;
            const char *data_ptr = e->dict->data ? e->dict->data : e->dict->index->mmap_data;
            if (!flat_index_entry_matches_query(data_ptr, res, normalized_query, strlen(normalized_query))) break;

            if (!dict_header_printed) {
                g_print("\n--- From: %s (%s) ---\n", e->name, dict_format_to_str(e->format));
                dict_header_printed = TRUE;
            }

            char *raw_hw = g_strndup(data_ptr + res->h_off, res->h_len);
            g_print("Headword: %s\n", raw_hw);
            g_free(raw_hw);

            char *rendered = render_entry_def_to_html(e, res);
            if (rendered) {
                /* Simple HTML tag stripping for CLI */
                GRegex *regex = g_regex_new("<[^>]*>", 0, 0, NULL);
                char *stripped = g_regex_replace(regex, rendered, -1, 0, "", 0, NULL);
                g_print("%s\n", stripped);
                g_free(stripped);
                g_regex_unref(regex);
                g_free(rendered);
            }
            
            total_found++;
            pos++;
            if (pos >= flat_index_count(e->dict->index)) break;
        }
    }

    if (total_found == 0) {
        g_print("\nNo exact match found for '%s'.\n", query);
    } else {
        g_print("\nFound %d matches.\n", total_found);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    // Disable compositing to fix rendering issues
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
    // Disable AT-SPI bridge to prevent GTK4 accessibility/tooltip crashes
    setenv("NO_AT_BRIDGE", "1", 1);
    // Completely disable GTK4 accessibility backend to prevent cleanup crashes on window close
    setenv("GTK_A11Y", "none", 1);
    // Seed random number generator
    srand(time(NULL));
    // Load settings first
    app_settings = settings_load();
    active_scope_id = g_strdup("all");
    history_words = load_word_list(HISTORY_FILE_NAME, 200);
    favorite_words = load_word_list(FAVORITES_FILE_NAME, 0);

    /* Parse CLI arguments */
    char *cli_query = NULL;
    char *in_dict = NULL;
    gboolean force_cli = FALSE;

    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--in-dict=")) {
            in_dict = argv[i] + 10;
            force_cli = TRUE;
        } else if (argv[i][0] == '-') {
            // Ignore other flags for now or handle them
        } else {
            if (!cli_query) cli_query = argv[i];
            // If it doesn't look like a path, it's a query
            struct stat st;
            if (stat(argv[i], &st) != 0 || !S_ISDIR(st.st_mode)) {
                force_cli = TRUE;
            }
        }
    }

    if (force_cli && cli_query) {
        int ret = run_cli_search(cli_query, in_dict);
        /* Cleanup and exit */
        if (app_settings) settings_free(app_settings);
        g_free(active_scope_id);
        dict_loader_free_list(all_dicts);
        return ret;
    }

    /* Legacy single file/dir loading for GUI mode */
    if (argc > 1 && !force_cli) {
        struct stat st;
        if (stat(argv[1], &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                all_dicts = dict_loader_scan_directory(argv[1]);
            } else {
                DictFormat fmt = dict_detect_format(argv[1]);
                DictMmap *d = dict_load_any(argv[1], fmt, NULL, 0);
                if (d) {
                    DictEntry *e = g_new0(DictEntry, 1);
                    e->magic = 0xDEADC0DE;
                    e->ref_count = 1;
                    const char *slash = strrchr(argv[1], '/');
                    const char *base = slash ? slash + 1 : argv[1];
                    e->name = g_strdup(base);
                    e->path = g_strdup(argv[1]);
                    e->dict_id = settings_make_dictionary_id(e->path);
                    e->format = fmt;
                    e->dict = d;
                    all_dicts = e;
                }
            }
        }
    }
    /* No else: settings-based dirs are loaded async in on_activate */

    /* Optional: support a quick CLI-only scan mode for debugging. If the
     * environment variable DICTION_SCAN_ONLY is set, scan the provided
     * directory (argv[1]) and print discovered names/paths to stderr, then exit.
     */
    if (getenv("DICTION_SCAN_ONLY")) {
        const char *scan_dir = NULL;
        if (argc > 1) scan_dir = argv[1];
        if (!scan_dir) {
            g_printerr("[SCAN_ONLY] No directory provided to scan.\n");
            return 1;
        }
        DictEntry *head = dict_loader_scan_directory(scan_dir);
        for (DictEntry *e = head; e; e = e->next) {
            g_printerr("[SCAN_ONLY] name='%s' path='%s'\n",
                        e->name ? e->name : "(null)", e->path ? e->path : "(null)");
        }
        dict_loader_free_list(head);
        return 0;
    }

    AdwApplication *app = adw_application_new("io.github.fastrizwaan.diction", G_APPLICATION_DEFAULT_FLAGS);

    // Add settings and about actions
    GSimpleAction *settings_action = g_simple_action_new("settings", NULL);
    g_signal_connect(settings_action, "activate", G_CALLBACK(show_settings_dialog), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(settings_action));
    g_object_unref(settings_action);

    GSimpleAction *about_action = g_simple_action_new("about", NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(show_about_dialog), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));
    g_object_unref(about_action);

    GSimpleAction *scan_action = g_simple_action_new("scan-clipboard", NULL);
    g_signal_connect(scan_action, "activate", G_CALLBACK(on_scan_clipboard_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(scan_action));
    g_object_unref(scan_action);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_app_shutdown), NULL);

    char *empty[] = { argv[0], NULL };
    int status = g_application_run(G_APPLICATION(app), 1, empty);

    // Save settings on exit
    if (app_settings) {
        settings_save(app_settings);
        settings_free(app_settings);
    }
    scan_popup_destroy();
    tray_icon_destroy();
    global_shortcut_destroy();
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
    }
    cancel_sidebar_search();
    free_word_list(&history_words);
    free_word_list(&favorite_words);
    g_free(active_scope_id);
    g_free(last_search_query);

    g_object_unref(app);
    set_active_entry(NULL);
    dict_loader_free_list(all_dicts);

    return status;
}
