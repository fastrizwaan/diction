#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <adwaita.h>
#include "langid.h"
#include "langpair.h"
#include <webkit/webkit.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "dict-mmap.h"
#include "dict-loader.h"
#include "dict-render.h"
#include "settings.h"
#include "scan-popup.h"
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
static GPtrArray *related_row_payloads = NULL;
static GHashTable *dictionary_dir_monitors = NULL;
static GHashTable *dictionary_root_parent_monitors = NULL;
static guint dictionary_watch_reload_source_id = 0;
static gboolean force_directory_rescan_requested = FALSE;
static WebKitUserContentManager *font_ucm = NULL;       /* shared across web views */
static WebKitUserStyleSheet *font_user_stylesheet = NULL; /* current injected font CSS */
static GtkWindow *main_window = NULL;
static void app_show_window(void);
static GtkWindow *startup_splash_window = NULL;
static GtkLabel *startup_splash_status_label = NULL;
static GtkLabel *startup_splash_count_label = NULL;
static GtkProgressBar *startup_splash_progress = NULL;
static guint startup_splash_pulse_id = 0;
static gboolean startup_loading_active = FALSE;
static gboolean dictionary_loading_in_progress = FALSE;
static gint64 rescan_suppress_until = 0;
static gboolean startup_random_word_pending = FALSE;

#define GLOBAL_SHORTCUT_ID "diction-scan-shortcut"
#define GLOBAL_SHORTCUT_TRIGGER "Super+Alt+L"

typedef enum {
    PORTAL_REQUEST_CREATE_SHORTCUT_SESSION = 1,
    PORTAL_REQUEST_BIND_SHORTCUTS = 2
} PortalRequestKind;

static GDBusConnection *global_shortcut_conn = NULL;
static char *global_shortcut_session_handle = NULL;
static guint global_shortcut_signal_sub_id = 0;
static guint global_shortcut_create_response_sub_id = 0;
static guint global_shortcut_bind_response_sub_id = 0;

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
} SidebarListView;

static SidebarListView dict_sidebar = {0};
static SidebarListView history_sidebar = {0};
static SidebarListView favorites_sidebar = {0};
static SidebarListView groups_sidebar = {0};
static GtkCssProvider *dynamic_theme_provider = NULL;

static gboolean pulse_startup_splash(gpointer user_data) {
    (void)user_data;
    if (!startup_loading_active || !startup_splash_progress) {
        startup_splash_pulse_id = 0;
        return G_SOURCE_REMOVE;
    }

    gtk_progress_bar_pulse(startup_splash_progress);
    return G_SOURCE_CONTINUE;
}

static void ensure_startup_splash_pulsing(void) {
    if (startup_splash_pulse_id != 0) {
        return;
    }

    if (!startup_splash_progress) {
        return;
    }

    gtk_progress_bar_set_pulse_step(startup_splash_progress, 0.08);
    startup_splash_pulse_id = g_timeout_add(80, pulse_startup_splash, NULL);
}

static void stop_startup_splash_pulsing(void) {
    if (startup_splash_pulse_id != 0) {
        g_source_remove(startup_splash_pulse_id);
        startup_splash_pulse_id = 0;
    }
}

static void update_startup_splash_progress(guint completed, guint total, const char *status_text) {
    if (!startup_splash_window) {
        return;
    }

    if (startup_splash_status_label) {
        if (status_text && *status_text) {
            gtk_label_set_text(startup_splash_status_label, status_text);
        } else if (total > 0) {
            char *fallback = g_strdup_printf("Loading dictionaries... %u/%u", completed, total);
            gtk_label_set_text(startup_splash_status_label, fallback);
            g_free(fallback);
        } else {
            gtk_label_set_text(startup_splash_status_label, "Preparing dictionary library...");
        }
    }

    if (!startup_splash_progress) {
        return;
    }

    if (total > 0) {
        if (startup_splash_count_label) {
            char *count = g_strdup_printf("%u of %u", completed, total);
            gtk_label_set_text(startup_splash_count_label, count);
            g_free(count);
        }
        stop_startup_splash_pulsing();
        gtk_progress_bar_set_fraction(startup_splash_progress,
            CLAMP((gdouble)completed / (gdouble)MAX(total, 1), 0.0, 1.0));
    } else {
        if (startup_splash_count_label) {
            gtk_label_set_text(startup_splash_count_label, "Scanning...");
        }
        gtk_progress_bar_set_fraction(startup_splash_progress, 0.0);
        ensure_startup_splash_pulsing();
    }
}

static GtkWidget *create_startup_splash_logo(void) {
    char *cwd = g_get_current_dir();
    char *icon_path = g_build_filename(cwd,
                                       "data", "icons",
                                       "io.github.fastrizwaan.diction.svg",
                                       NULL);
    GtkWidget *image = NULL;

    if (g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
        image = gtk_image_new_from_file(icon_path);
    } else {
        image = gtk_image_new_from_icon_name("io.github.fastrizwaan.diction");
    }

    gtk_image_set_pixel_size(GTK_IMAGE(image), 48);
    g_free(cwd);
    g_free(icon_path);
    return image;
}

/* Request cancellation of the current loader generation (called from UI). */
void request_loader_cancel(void) {
    g_atomic_int_inc(&loader_generation);
    g_mutex_lock(&loader_cancel_mutex);
    if (loader_cancellable) {
        g_cancellable_cancel(loader_cancellable);
    }
    g_mutex_unlock(&loader_cancel_mutex);
}

static void ensure_startup_splash_css(void) {
    static GtkCssProvider *provider = NULL;
    if (provider) {
        return;
    }

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "window.startup-splash {"
        "  background: transparent;"
        "}"
        ".startup-shell {"
        "  margin: 18px;"
        "  border-radius: 28px;"
        "  padding: 32px 36px;"
        "  border: 1px solid alpha(@accent_bg_color, 0.2);"
        "  background: linear-gradient(135deg, alpha(@accent_bg_color, 0.12), @window_bg_color);"
        "  box-shadow: 0 16px 48px rgba(0,0,0,0.22);"
        "}"
        ".startup-logo-wrap {"
        "  min-width: 88px;"
        "  min-height: 88px;"
        "  border-radius: 24px;"
        "  background: alpha(@accent_bg_color, 0.16);"
        "  box-shadow: inset 0 2px 4px rgba(255,255,255,0.1);"
        "}"
        ".startup-title {"
        "  font-size: 1.85rem;"
        "  font-weight: 800;"
        "  letter-spacing: -0.03em;"
        "  color: @accent_color;"
        "}"
        ".startup-subtitle {"
        "  opacity: 0.75;"
        "  font-size: 1.0rem;"
        "  line-height: 1.4;"
        "}"
        ".startup-meta {"
        "  min-height: 24px;"
        "  margin-top: 20px;"
        "}"
        ".startup-status {"
        "  opacity: 0.85;"
        "  font-size: 0.94rem;"
        "  font-weight: 500;"
        "}"
        ".startup-count {"
        "  opacity: 0.65;"
        "  font-size: 0.88rem;"
        "  font-variant-numeric: tabular-nums;"
        "}"
        ".startup-progress trough {"
        "  background: alpha(@accent_bg_color, 0.1);"
        "  border: none;"
        "}"
        ".startup-progress progress {"
        "  background: @accent_bg_color;"
        "  border: none;"
        "  min-height: 8px;"
        "  border-radius: 999px;"
        "}"
        ".startup-stop-btn {"
        "  margin-left: 16px;"
        "  padding: 8px;"
        "  border-radius: 12px;"
        "  transition: all 0.2s ease;"
        "}"
        ".startup-stop-btn:hover {"
        "  background: alpha(@error_bg_color, 0.15);"
        "  color: @error_color;"
        "}");

    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

static void show_startup_splash(GtkApplication *app) {
    if (startup_splash_window) {
        return;
    }

    ensure_startup_splash_css();

    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_application(window, app);
    gtk_window_set_title(window, "Diction");
    gtk_window_set_default_size(window, 520, 200);
    gtk_window_set_resizable(window, FALSE);
    gtk_window_set_decorated(window, FALSE);
    gtk_window_set_modal(window, FALSE);
    gtk_window_set_hide_on_close(window, TRUE);
    gtk_window_set_icon_name(window, "io.github.fastrizwaan.diction");
    if (main_window) {
        gtk_window_set_transient_for(window, main_window);
    }
    gtk_widget_add_css_class(GTK_WIDGET(window), "startup-splash");

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(outer, "startup-shell");
    gtk_window_set_child(window, outer);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_append(GTK_BOX(outer), header);

    GtkWidget *logo_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(logo_wrap, "startup-logo-wrap");
    gtk_widget_set_halign(logo_wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(logo_wrap, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), logo_wrap);
    gtk_box_append(GTK_BOX(logo_wrap), create_startup_splash_logo());

    GtkWidget *copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(copy, TRUE);
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), copy);

    GtkWidget *title = gtk_label_new("Diction");
    gtk_widget_add_css_class(title, "startup-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(copy), title);

    GtkWidget *stop_btn = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    gtk_widget_add_css_class(stop_btn, "flat");
    gtk_widget_add_css_class(stop_btn, "startup-stop-btn");
    gtk_widget_set_valign(stop_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(stop_btn, "Stop and Load App");
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(request_loader_cancel), NULL);
    gtk_box_append(GTK_BOX(header), stop_btn);

    GtkWidget *subtitle = gtk_label_new("A fast and lightweight dictionary application");
    gtk_widget_add_css_class(subtitle, "startup-subtitle");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
    gtk_box_append(GTK_BOX(copy), subtitle);

    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(meta, "startup-meta");
    gtk_widget_set_margin_top(meta, 14);
    gtk_box_append(GTK_BOX(outer), meta);

    startup_splash_status_label = GTK_LABEL(gtk_label_new("Preparing dictionary library..."));
    gtk_widget_add_css_class(GTK_WIDGET(startup_splash_status_label), "startup-status");
    gtk_label_set_xalign(startup_splash_status_label, 0.0f);
    gtk_label_set_ellipsize(startup_splash_status_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(startup_splash_status_label), TRUE);
    gtk_box_append(GTK_BOX(meta), GTK_WIDGET(startup_splash_status_label));

    startup_splash_count_label = GTK_LABEL(gtk_label_new("Scanning..."));
    gtk_widget_add_css_class(GTK_WIDGET(startup_splash_count_label), "startup-count");
    gtk_label_set_xalign(startup_splash_count_label, 1.0f);
    gtk_box_append(GTK_BOX(meta), GTK_WIDGET(startup_splash_count_label));

    startup_splash_progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_add_css_class(GTK_WIDGET(startup_splash_progress), "startup-progress");
    gtk_widget_set_margin_top(GTK_WIDGET(startup_splash_progress), 12);
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(startup_splash_progress));

    startup_splash_window = window;
    startup_loading_active = TRUE;
    update_startup_splash_progress(0, 0, "Preparing dictionary library...");
    gtk_window_present(window);
}

static void close_startup_splash(void) {
    stop_startup_splash_pulsing();
    startup_loading_active = FALSE;

    if (startup_splash_window) {
        gtk_window_destroy(startup_splash_window);
    }

    startup_splash_window = NULL;
    startup_splash_status_label = NULL;
    startup_splash_count_label = NULL;
    startup_splash_progress = NULL;
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
    guint source_id;
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

static gboolean spawn_audio_argv(const char *const *argv, const char *label) {
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL,
                                (char **)argv,
                                NULL,
                                G_SPAWN_SEARCH_PATH |
                                G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL,
                                NULL,
                                NULL,
                                &error);
    if (ok) {
        fprintf(stderr, "[AUDIO PLAY] Playing with '%s'...\n", label);
        return TRUE;
    }

    g_clear_error(&error);
    return FALSE;
}

static gboolean spawn_audio_shell_command(const char *command, const char *label) {
    const char *argv[] = { "/bin/sh", "-c", command, NULL };
    return spawn_audio_argv(argv, label);
}

static gboolean path_has_extension(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && g_ascii_strcasecmp(dot, ext) == 0;
}

static gboolean looks_like_url(const char *path) {
    return path && (g_str_has_prefix(path, "http://") || g_str_has_prefix(path, "https://"));
}

static gboolean play_audio_via_pcm_pipeline(const char *audio_path) {
    if (!g_find_program_in_path("ffmpeg")) {
        return FALSE;
    }

    char *quoted = g_shell_quote(audio_path);
    gboolean ok = FALSE;

    if (g_find_program_in_path("pw-play")) {
        char *cmd = g_strdup_printf(
            "ffmpeg -nostdin -loglevel error -i %s -f s16le -acodec pcm_s16le -ac 2 -ar 48000 - | "
            "pw-play --raw --format s16 --channels 2 --rate 48000 -",
            quoted);
        ok = spawn_audio_shell_command(cmd, "ffmpeg | pw-play");
        g_free(cmd);
    } else if (g_find_program_in_path("aplay")) {
        char *cmd = g_strdup_printf(
            "ffmpeg -nostdin -loglevel error -i %s -f s16le -acodec pcm_s16le -ac 2 -ar 48000 - | "
            "aplay -q -f S16_LE -c 2 -r 48000 -",
            quoted);
        ok = spawn_audio_shell_command(cmd, "ffmpeg | aplay");
        g_free(cmd);
    }

    g_free(quoted);
    return ok;
}

static gboolean play_audio_via_gstreamer(const char *audio_path, gboolean is_spx) {
    if (!g_find_program_in_path("gst-launch-1.0")) {
        return FALSE;
    }

    gboolean ok = FALSE;
    char *uri = NULL;
    GError *error = NULL;

    if (looks_like_url(audio_path)) {
        uri = g_strdup(audio_path);
    } else {
        uri = g_filename_to_uri(audio_path, NULL, &error);
        if (!uri) {
            g_clear_error(&error);
            return FALSE;
        }
    }

    const guint buffer_size = is_spx ? 262144 : 65536;
    char *quoted_uri = g_shell_quote(uri);
    char *cmd = g_strdup_printf(
        "gst-launch-1.0 -q playbin uri=%s audio-sink='queue max-size-bytes=%u ! autoaudiosink'",
        quoted_uri, buffer_size);

    ok = spawn_audio_shell_command(cmd, "gst-playbin");

    g_free(cmd);
    g_free(quoted_uri);
    g_free(uri);
    return ok;
}

static void play_audio_file(const char *audio_path) {
    fprintf(stderr, "[AUDIO PLAY] Attempting to play: %s\n", audio_path);

    /* Prefer GStreamer-based playback which handles many formats
     * and allows configuring a larger internal queue for `.spx` files
     * to reduce stuttering. Fall back to ffmpeg->pw-play/aplay if needed. */
    if (play_audio_via_gstreamer(audio_path, path_has_extension(audio_path, ".spx"))) {
        return;
    }

    if (!looks_like_url(audio_path) &&
        (path_has_extension(audio_path, ".spx") ||
         path_has_extension(audio_path, ".ogg") ||
         path_has_extension(audio_path, ".oga"))) {
        if (play_audio_via_pcm_pipeline(audio_path)) {
            return;
        }
    }

    if (g_find_program_in_path("ffplay")) {
        const char *argv[] = { "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", audio_path, NULL };
        if (spawn_audio_argv(argv, "ffplay")) {
            return;
        }
    }

    if (g_find_program_in_path("mpg123")) {
        const char *argv[] = { "mpg123", "-q", audio_path, NULL };
        if (spawn_audio_argv(argv, "mpg123")) {
            return;
        }
    }

    if (g_find_program_in_path("play")) {
        const char *argv[] = { "play", "-q", audio_path, NULL };
        if (spawn_audio_argv(argv, "play")) {
            return;
        }
    }

    if (g_find_program_in_path("paplay")) {
        const char *argv[] = { "paplay", audio_path, NULL };
        if (spawn_audio_argv(argv, "paplay")) {
            return;
        }
    }

    fprintf(stderr, "[AUDIO ERROR] No usable audio player found\n");
}

static char *query_param_dup(const char *query, const char *key) {
    if (!query || !key) {
        return NULL;
    }

    char **pairs = g_strsplit(query, "&", -1);
    char *value = NULL;

    for (int i = 0; pairs[i]; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        if (strcmp(pairs[i], key) == 0) {
            value = g_uri_unescape_string(eq + 1, NULL);
            *eq = '=';
            break;
        }
        *eq = '=';
    }

    g_strfreev(pairs);
    return value;
}

static gboolean try_play_encoded_sound_uri(const char *uri) {
    const char *query = strchr(uri, '?');
    if (!query) {
        return FALSE;
    }

    char *audio_url = query_param_dup(query + 1, "url");
    char *audio_path_param = query_param_dup(query + 1, "path");
    char *resource_dir = query_param_dup(query + 1, "dir");
    char *sound_file = query_param_dup(query + 1, "file");

    if (audio_url && *audio_url) {
        fprintf(stderr, "[AUDIO CLICKED] URL: %s\n", audio_url);
        play_audio_file(audio_url);
        g_free(audio_url);
        g_free(audio_path_param);
        g_free(resource_dir);
        g_free(sound_file);
        return TRUE;
    }

    g_free(audio_url);

    if (audio_path_param && *audio_path_param) {
        fprintf(stderr, "[AUDIO CLICKED] Path: %s\n", audio_path_param);
        if (g_file_test(audio_path_param, G_FILE_TEST_EXISTS)) {
            play_audio_file(audio_path_param);
        } else {
            fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path_param);
        }
        g_free(audio_path_param);
        g_free(resource_dir);
        g_free(sound_file);
        return TRUE;
    }

    g_free(audio_path_param);

    if (!resource_dir || !sound_file) {
        g_free(resource_dir);
        g_free(sound_file);
        return FALSE;
    }

    fprintf(stderr, "[AUDIO CLICKED] Resource dir: %s\n", resource_dir);
    fprintf(stderr, "[AUDIO CLICKED] File: %s\n", sound_file);

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

    if (!audio_path) {
        audio_path = g_build_filename(resource_dir, sound_file, NULL);
    }

    if (audio_path && g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
        play_audio_file(audio_path);
    } else {
        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path ? audio_path : sound_file);
    }

    g_free(audio_path);
    g_free(resource_dir);
    g_free(sound_file);
    return TRUE;
}

static char *sanitize_user_word(const char *value) {
    if (!value) {
        return NULL;
    }

    char *valid = g_utf8_make_valid(value, -1);
    char *text = g_strdup(valid);
    g_free(valid);
    g_strstrip(text);
    for (char *p = text; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            *p = ' ';
        }
    }
    g_strstrip(text);

    if (!*text || strlen(text) > 256) {
        g_free(text);
        return NULL;
    }

    GString *clean = g_string_new("");
    for (const char *p = text; *p; p++) {
        if (g_ascii_isprint(*p) || g_ascii_isspace(*p) || ((unsigned char)*p >= 0x80)) {
            g_string_append_c(clean, *p);
        }
    }
    g_free(text);
    g_strstrip(clean->str);
    if (!*clean->str) {
        g_string_free(clean, TRUE);
        return NULL;
    }
    return g_string_free(clean, FALSE);
}

static gboolean text_has_replacement_char(const char *text) {
    return text && strstr(text, "\xEF\xBF\xBD") != NULL;
}

static gboolean dsl_headword_is_escapable_char(char c) {
    return c != '\0' && strchr(" {}~\\@#()[]<>;", c) != NULL;
}

static size_t dsl_headword_brace_tag_len(const char *text) {
    static const char *patterns[] = {
        "{*}",
        "{·}",
        "{ˈ}",
        "{ˌ}",
        "{[']}",
        "{[/']}"
    };

    if (!text || text[0] != '{') {
        return 0;
    }

    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        if (g_str_has_prefix(text, patterns[i])) {
            return strlen(patterns[i]);
        }
    }

    return 0;
}

static gboolean search_query_needs_literal_prefilter_bypass(const char *query) {
    if (!query) {
        return FALSE;
    }

    for (const char *p = query; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isspace(ch) || g_unichar_isalnum(ch)) {
            continue;
        }
        return TRUE;
    }

    return FALSE;
}

static char *normalize_headword_for_search(const char *value, gboolean unescape_dsl) {
    if (!value) {
        return NULL;
    }

    char *valid = g_utf8_make_valid(value, -1);
    GString *out = g_string_new("");
    const char *p = valid;

    while (*p) {
        if (*p == '{') {
            size_t brace_tag_len = dsl_headword_brace_tag_len(p);
            if (brace_tag_len > 0) {
                p += brace_tag_len;
                continue;
            }
            if (unescape_dsl) {
                p++;
                continue;
            }
        }

        /* Raw DSL markers that should be ignored even outside of braces */
        if (*p == '*') { p++; continue; }

        /* UTF-8 middle dot (C2 B7) */
        if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB7) {
            p += 2;
            continue;
        }

        /* UTF-8 Stress marks (IPA CB 88, CB 8C) */
        if ((unsigned char)p[0] == 0xCB && ((unsigned char)p[1] == 0x88 || (unsigned char)p[1] == 0x8C)) {
            p += 2;
            continue;
        }

        /* DSL-specific square bracket tags in headwords (rare handles formatting) */
        if (g_str_has_prefix(p, "[']")) { p += 3; continue; }
        if (g_str_has_prefix(p, "[/']")) { p += 4; continue; }

        /* Strip actual Unicode combining acute accent (U+0301) for search */
        if (g_str_has_prefix(p, "\xCC\x81")) { p += 2; continue; }

        if (*p == '}' && unescape_dsl) {
            p++;
            continue;
        }



        if (*p == '\\' && p[1] != '\0') {
            if (unescape_dsl) {
                /* Only unescape DSL control escapes; preserve literal leet/backslash patterns. */
                if (dsl_headword_is_escapable_char(p[1])) {
                    const char *next = p + 1;
                    const char *next_end = g_utf8_next_char(next);
                    g_string_append_len(out, next, next_end - next);
                    p = next_end;
                } else {
                    /* Not special, keep the backslash */
                    g_string_append_c(out, '\\');
                    p++;
                }
            } else {
                /* Literal mode: keep everything as-is (e.g. from user search box) */
                g_string_append_c(out, '\\');
                p++;
            }
            continue;
        }

        if (g_ascii_isspace(*p)) {
            g_string_append_c(out, ' ');
            while (g_ascii_isspace(*p)) p++;
            continue;
        }

        const char *next = g_utf8_next_char(p);
        g_string_append_len(out, p, next - p);
        p = next;
    }

    char *normalized = g_string_free(out, FALSE);
    g_free(valid);

    char *trimmed = g_strstrip(normalized);
    if (!trimmed || *trimmed == '\0') {
        g_free(normalized);
        return NULL;
    }
    char *final = g_strdup(trimmed);
    g_free(normalized);
    return final;
}

typedef enum {
    SEARCH_BUCKET_EXACT = 0,
    SEARCH_BUCKET_SUFFIX,
    SEARCH_BUCKET_PREFIX,
    SEARCH_BUCKET_PHRASE,
    SEARCH_BUCKET_SUBSTRING,
    SEARCH_BUCKET_FUZZY
} SearchBucket;

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

static guint utf8_length_or_bytes(const char *text) {
    if (!text || !*text) {
        return 0;
    }
    return (guint)g_utf8_strlen(text, -1);
}

static guint gestalt_longest_match(const gunichar *a, guint a_start, guint a_end,
                                   const gunichar *b, guint b_start, guint b_end,
                                   guint *out_a, guint *out_b) {
    guint max_len = 0;
    guint best_a = a_start;
    guint best_b = b_start;

    for (guint i = a_start; i < a_end; i++) {
        for (guint j = b_start; j < b_end; j++) {
            if (a[i] == b[j]) {
                guint len = 1;
                while (i + len < a_end && j + len < b_end && a[i + len] == b[j + len]) {
                    len++;
                }
                if (len > max_len) {
                    max_len = len;
                    best_a = i;
                    best_b = j;
                }
            }
        }
    }
    *out_a = best_a;
    *out_b = best_b;
    return max_len;
}

static guint gestalt_matches(const gunichar *a, guint a_start, guint a_end,
                             const gunichar *b, guint b_start, guint b_end) {
    if (a_start >= a_end || b_start >= b_end) {
        return 0;
    }
    guint out_a, out_b;
    guint match_len = gestalt_longest_match(a, a_start, a_end, b, b_start, b_end, &out_a, &out_b);
    if (match_len == 0) {
        return 0;
    }
    guint matches = match_len;
    matches += gestalt_matches(a, a_start, out_a, b, b_start, out_b);
    matches += gestalt_matches(a, out_a + match_len, a_end, b, out_b + match_len, b_end);
    return matches;
}

static double sequence_matcher_ratio(const char *str_a, const char *str_b) {
    if (!str_a || !str_b) return 0.0;
    
    glong len_a_chars = 0, len_b_chars = 0;
    gunichar *a = g_utf8_to_ucs4_fast(str_a, -1, &len_a_chars);
    gunichar *b = g_utf8_to_ucs4_fast(str_b, -1, &len_b_chars);
    
    if (!a || !b) {
        g_free(a);
        g_free(b);
        return 0.0;
    }
    
    if (len_a_chars == 0 && len_b_chars == 0) {
        g_free(a); g_free(b);
        return 1.0;
    }
    if (len_a_chars == 0 || len_b_chars == 0) {
        g_free(a); g_free(b);
        return 0.0;
    }
    
    guint matches = gestalt_matches(a, 0, len_a_chars, b, 0, len_b_chars);
    g_free(a);
    g_free(b);
    return (2.0 * matches) / (double)(len_a_chars + len_b_chars);
}

static const char *candidate_key_without_definite_article(const char *candidate_key) {
    if (!candidate_key) {
        return NULL;
    }

    if (g_str_has_prefix(candidate_key, "the ")) {
        return candidate_key + 4;
    }

    return NULL;
}

static int search_bucket_rank(SearchBucket bucket) {
    return (int)bucket;
}

static char *collapse_search_separators(const char *text) {
    if (!text) {
        return NULL;
    }

    GString *out = g_string_sized_new(strlen(text));
    gboolean changed = FALSE;

    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isspace(ch) || ch == '-' || ch == '_' || ch == '/' || ch == '.' || ch == 0x00B7) {
            changed = TRUE;
            continue;
        }

        const char *next = g_utf8_next_char(p);
        g_string_append_len(out, p, next - p);
    }

    if (!changed) {
        g_string_free(out, TRUE);
        return NULL;
    }

    return g_string_free(out, FALSE);
}

static gboolean classify_search_candidate(const char *query_key,
                                          guint query_len,
                                          const char *candidate_key,
                                          SearchBucket *bucket_out,
                                          double *fuzzy_score_out) {
    if (!query_key || !candidate_key || !*candidate_key) {
        if (candidate_key && *candidate_key && query_key && !*query_key) {
            if (bucket_out) *bucket_out = SEARCH_BUCKET_PREFIX;
            if (fuzzy_score_out) *fuzzy_score_out = 0.0;
            return TRUE;
        }
        return FALSE;
    }

    /* 1. EXACT */
    if (g_strcmp0(candidate_key, query_key) == 0) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_EXACT;
        if (fuzzy_score_out) *fuzzy_score_out = 1.0;
        return TRUE;
    }

    /* 2. SUFFIX */
    if (g_str_has_suffix(candidate_key, query_key)) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_SUFFIX;
        if (fuzzy_score_out) *fuzzy_score_out = 0.0;
        return TRUE;
    }

    /* 3. PREFIX */
    if (g_str_has_prefix(candidate_key, query_key)) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_PREFIX;
        if (fuzzy_score_out) *fuzzy_score_out = 0.0;
        return TRUE;
    }

    /* 4. PHRASE / 5. SUBSTRING */
    const char *match = strstr(candidate_key, query_key);
    gboolean is_phrase = FALSE;
    gboolean is_substring = FALSE;

    if (match != NULL) {
        is_substring = TRUE;
        gsize qlen = strlen(query_key);
        const char *m = match;
        
        while (m != NULL) {
            char before = (m > candidate_key) ? *(m - 1) : '\0';
            char after = *(m + qlen);
            
            gboolean valid_before = (before == '\0' || before == ' ' || before == '-' || before == '_' || before == '/');
            gboolean valid_after = (after == '\0' || after == ' ' || after == '-' || after == '_' || after == '/');
            
            if (valid_before && valid_after) {
                is_phrase = TRUE;
                break;
            }
            m = strstr(m + 1, query_key);
        }
        
        if (is_phrase) {
            if (bucket_out) *bucket_out = SEARCH_BUCKET_PHRASE;
            if (fuzzy_score_out) *fuzzy_score_out = 0.0;
            return TRUE;
        }
    }

    /* 5. SUBSTRING */
    if (is_substring) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_SUBSTRING;
        if (fuzzy_score_out) *fuzzy_score_out = 0.0;
        return TRUE;
    }

    /* 6. FUZZY */
    if (query_len < 3) {
        return FALSE;
    }

    if (query_len >= 3 && strlen(candidate_key) > 32) {
        return FALSE; // skip expensive fuzzy early
    }

    guint candidate_len = utf8_length_or_bytes(candidate_key);
    guint length_delta = candidate_len > query_len ? candidate_len - query_len : query_len - candidate_len;
    if (length_delta > MAX(6U, query_len)) {
        return FALSE;
    }

    double ratio = sequence_matcher_ratio(query_key, candidate_key);
    double min_ratio = query_len >= 5 ? 0.74 : 0.78;
    if (ratio < min_ratio) {
        return FALSE;
    }

    if (bucket_out) *bucket_out = SEARCH_BUCKET_FUZZY;
    if (fuzzy_score_out) *fuzzy_score_out = ratio;
    return TRUE;
}

static gboolean classify_search_candidate_flexible(const char *query_key,
                                                   guint query_len,
                                                   const char *query_compact_key,
                                                   guint query_compact_len,
                                                   const char *candidate_key,
                                                   SearchBucket *bucket_out,
                                                   double *fuzzy_score_out) {
    SearchBucket best_bucket = SEARCH_BUCKET_FUZZY;
    double best_score = 0.0;
    gboolean found = classify_search_candidate(query_key, query_len, candidate_key, &best_bucket, &best_score);

    const char *articleless = candidate_key_without_definite_article(candidate_key);
    if (articleless && *articleless) {
        SearchBucket alt_bucket = SEARCH_BUCKET_FUZZY;
        double alt_score = 0.0;
        gboolean alt_found = classify_search_candidate(query_key, query_len, articleless, &alt_bucket, &alt_score);
        if (alt_found &&
            (!found ||
             search_bucket_rank(alt_bucket) < search_bucket_rank(best_bucket) ||
             (search_bucket_rank(alt_bucket) == search_bucket_rank(best_bucket) && alt_score > best_score))) {
            best_bucket = alt_bucket;
            best_score = alt_score;
            found = TRUE;
        }
    }

    if (query_compact_key && *query_compact_key) {
        char *compact_candidate = collapse_search_separators(candidate_key);
        if (compact_candidate && *compact_candidate) {
            SearchBucket alt_bucket = SEARCH_BUCKET_FUZZY;
            double alt_score = 0.0;
            gboolean alt_found = classify_search_candidate(query_compact_key, query_compact_len, compact_candidate, &alt_bucket, &alt_score);
            if (alt_found &&
                (!found ||
                 search_bucket_rank(alt_bucket) < search_bucket_rank(best_bucket) ||
                 (search_bucket_rank(alt_bucket) == search_bucket_rank(best_bucket) && alt_score > best_score))) {
                best_bucket = alt_bucket;
                best_score = alt_score;
                found = TRUE;
            }

            const char *compact_articleless = candidate_key_without_definite_article(compact_candidate);
            if (compact_articleless && *compact_articleless) {
                alt_bucket = SEARCH_BUCKET_FUZZY;
                alt_score = 0.0;
                alt_found = classify_search_candidate(query_compact_key, query_compact_len, compact_articleless, &alt_bucket, &alt_score);
                if (alt_found &&
                    (!found ||
                     search_bucket_rank(alt_bucket) < search_bucket_rank(best_bucket) ||
                     (search_bucket_rank(alt_bucket) == search_bucket_rank(best_bucket) && alt_score > best_score))) {
                    best_bucket = alt_bucket;
                    best_score = alt_score;
                    found = TRUE;
                }
            }
        }
        g_free(compact_candidate);
    }

    if (found) {
        if (bucket_out) *bucket_out = best_bucket;
        if (fuzzy_score_out) *fuzzy_score_out = best_score;
    }

    return found;
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

static void sidebar_search_state_free(SidebarSearchState *state) {
    if (!state) {
        return;
    }
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

static gboolean word_list_contains_ci(GPtrArray *list, const char *word) {
    if (!list || !word) {
        return FALSE;
    }
    for (guint i = 0; i < list->len; i++) {
        const char *item = g_ptr_array_index(list, i);
        if (g_ascii_strcasecmp(item, word) == 0) {
            return TRUE;
        }
    }
    return FALSE;
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
            if (word_list_contains_ci(words, word)) {
                g_free(word);
                continue;
            }
            g_ptr_array_add(words, word);
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
}

static void set_related_rows(GPtrArray *labels, GPtrArray *payloads) {
    clear_related_rows();
    if (!labels || labels->len == 0 || !GTK_IS_STRING_LIST(related_string_list) || !related_row_payloads) {
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
    g_ptr_array_set_size(payloads, 0);
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
    g_ptr_array_set_size(payloads, 0);
}

static void clear_sidebar_list(SidebarListView *sidebar) {
    if (!sidebar) {
        return;
    }
    if (sidebar->payloads) {
        g_ptr_array_set_size(sidebar->payloads, 0);
    }
    if (GTK_IS_STRING_LIST(sidebar->string_list)) {
        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(sidebar->string_list));
        gtk_string_list_splice(sidebar->string_list, 0, n_items, NULL);
    }
}

static void set_sidebar_list_rows(SidebarListView *sidebar, GPtrArray *labels, GPtrArray *payloads) {
    clear_sidebar_list(sidebar);
    if (!sidebar || !labels || labels->len == 0 || !GTK_IS_STRING_LIST(sidebar->string_list) || !sidebar->payloads) {
        return;
    }

    for (guint i = 0; i < payloads->len; i++) {
        g_ptr_array_add(sidebar->payloads, g_ptr_array_index(payloads, i));
    }
    g_ptr_array_set_size(payloads, 0);

    char **items = g_new0(char *, labels->len + 1);
    for (guint i = 0; i < labels->len; i++) {
        items[i] = g_ptr_array_index(labels, i);
    }
    gtk_string_list_splice(sidebar->string_list, 0, 0, (const char * const *)items);
    g_free(items);
}

static SidebarRowPayload *sidebar_payload_at(SidebarListView *sidebar, guint position) {
    if (!sidebar || !sidebar->payloads || position >= sidebar->payloads->len) {
        return NULL;
    }
    return g_ptr_array_index(sidebar->payloads, position);
}

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
    (void)binding;
    gboolean selected = g_value_get_boolean(from_value);
    const char *word = user_data;
    gboolean is_favorite = word && word_list_contains_ci(favorite_words, word);
    g_value_set_boolean(to_value, selected || is_favorite);
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

static void sidebar_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(box, "sidebar-row");
    gtk_widget_set_margin_start(box, 12);

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
    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    const char *title    = payload && payload->title    ? payload->title    : "";
    const char *subtitle = payload && payload->subtitle ? payload->subtitle : "";
    char *safe_title    = safe_markup_escape_n(title, -1);
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

        g_object_bind_property_full(item, "selected", star_btn, "visible", 
            G_BINDING_SYNC_CREATE,
            transform_sidebar_star_visibility, NULL, g_strdup(title), g_free);
    } else {
        gtk_widget_set_visible(star_btn, FALSE);
    }

    g_free(markup);
    g_free(safe_title);
    g_free(safe_subtitle);
}

static void populate_history_sidebar(void);
static void populate_favorites_sidebar(void);
static void populate_groups_sidebar(void);
static void populate_search_sidebar(const char *query);
static gboolean dict_entry_in_active_scope(DictEntry *entry);

static void cancel_sidebar_search(void) {
    if (sidebar_search_state && sidebar_search_state->source_id != 0) {
        g_source_remove(sidebar_search_state->source_id);
        sidebar_search_state->source_id = 0;
    }
    g_clear_pointer(&sidebar_search_state, sidebar_search_state_free);
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

static gboolean continue_sidebar_search(gpointer user_data) {
    SidebarSearchState *state = user_data;
    if (!state || state != sidebar_search_state) {
        return G_SOURCE_REMOVE;
    }

    guint processed = 0;
    const guint max_batch_size = 512;
    gint64 deadline_us = g_get_monotonic_time() + 2000;
    while (processed < max_batch_size && g_get_monotonic_time() < deadline_us) {
        if ((!state->search_entries || state->current_entry_index >= state->search_entries->len) &&
            !state->has_current_pos) {
            // END OF SEARCH - DO GLOBAL SORT & FLUSH
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

                if (n == 0) continue;

                if (!state->list_started) {
                    set_related_rows(state->global_bucket_labels[i], state->global_bucket_payloads[i]);
                    state->list_started = TRUE;
                } else {
                    append_related_rows(state->global_bucket_labels[i], state->global_bucket_payloads[i]);
                }
                g_ptr_array_set_size(state->global_bucket_labels[i], 0);
                g_ptr_array_set_size(state->global_bucket_payloads[i], 0);
            }

            if (!state->list_started &&
                g_hash_table_size(state->seen_words) == 0) {
                if (state->fts_limited) {
                    char *subtitle = g_strdup_printf("Searched %u of %u dictionaries in this scope.",
                                                     state->searched_dict_count,
                                                     state->scoped_dict_count);
                    populate_search_sidebar_status("No results", subtitle);
                    g_free(subtitle);
                } else {
                    populate_search_sidebar_status("No results", NULL);
                }
            }

            state->source_id = 0;
            g_clear_pointer(&sidebar_search_state, sidebar_search_state_free);
            return G_SOURCE_REMOVE;
        }

        if (!state->has_current_pos) {
            gboolean found_dict = FALSE;
            while (state->search_entries &&
                   state->current_entry_index < state->search_entries->len) {
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

            if (!found_dict) {
                continue;
            }
        }

        if (!state->has_current_pos) {
            continue;
        }
        if (!state->current_dict) {
            state->has_current_pos = FALSE;
            processed++;
            continue;
        }

        const FlatTreeEntry *node = NULL;
        if (state->is_fts) {
            if (!state->fts_regex) {
                state->has_current_pos = FALSE;
                continue;
            }
            size_t match_pos = flat_index_search_fts(state->current_dict->dict->index, state->fts_regex, state->current_pos);
            if (match_pos == (size_t)-1) {
                processed += state->current_dict_count - state->current_pos;
                state->has_current_pos = FALSE;
                continue;
            }
            processed += (match_pos - state->current_pos) + 1;
            state->current_pos = match_pos;
            node = flat_index_get(state->current_dict->dict->index, state->current_pos);
            state->current_pos++;
            if (state->current_pos >= state->current_dict_count) {
                state->has_current_pos = FALSE;
            }
        } else {
            node = flat_index_get(state->current_dict->dict->index, state->current_pos);
            if (!node) {
                state->has_current_pos = FALSE;
                continue;
            }
            state->current_pos++;
            if (state->current_pos >= state->current_dict_count) {
                state->has_current_pos = FALSE;
            }

            // FAST PRE-FILTER (zero alloc, length-safe)
            if (!state->skip_fast_prefilter &&
                !fast_strncasestr(state->current_dict->dict->data + node->h_off, node->h_len, state->query)) {
                processed++;
                if ((processed & 63) == 0) {
                    if (g_get_monotonic_time() > deadline_us)
                        break;
                }
                continue;
            }
        }

        char *word = g_strndup(state->current_dict->dict->data + node->h_off, node->h_len);
        char *clean_word = normalize_headword_for_search(word, TRUE);
        if (!clean_word || text_has_replacement_char(clean_word)) {
            g_free(word);
            g_free(clean_word);
            processed++;
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
                if (!g_hash_table_contains(state->seen_words, word_key)) {
                    RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                    payload->type = RELATED_ROW_CANDIDATE;
                    
                    /* Use rendering normalization for display in sidebar (strip dots) */
                    char *display_word = normalize_headword_for_render(word, node->h_len, TRUE);
                    payload->word = g_strdup(word);
                    payload->sort_key = g_utf8_casefold(display_word ? display_word : "", -1);
                    
                    payload->fuzzy_score = fuzzy_score;
                    g_hash_table_add(state->seen_words, g_strdup(word_key));
                
                int b = (int)bucket;
                if (b >= 0 && b < BUCKET_COUNT) {
                    g_ptr_array_add(state->global_bucket_labels[b], display_word); 
                    g_ptr_array_add(state->global_bucket_payloads[b], payload);
                    display_word = NULL;

                    // 🔥 PROGRESSIVE FLUSH (per bucket)
                    if ((state->global_bucket_labels[b]->len & 31) == 0) {
                        guint n = state->global_bucket_labels[b]->len;
                        if (n > 1) {
                            BucketItem *items = g_new(BucketItem, n);
                            for (guint j = 0; j < n; j++) {
                                RelatedRowPayload *row_payload = g_ptr_array_index(state->global_bucket_payloads[b], j);
                                items[j].label = g_ptr_array_index(state->global_bucket_labels[b], j);
                                items[j].sort_key = row_payload ? row_payload->sort_key : NULL;
                                items[j].payload = row_payload;
                                items[j].score = items[j].payload ? items[j].payload->fuzzy_score : 0.0;
                            }

                            g_sort_array(items, n, sizeof(BucketItem), compare_bucket_item, GINT_TO_POINTER(b));

                            for (guint j = 0; j < n; j++) {
                                g_ptr_array_index(state->global_bucket_labels[b], j) = items[j].label;
                                g_ptr_array_index(state->global_bucket_payloads[b], j) = items[j].payload;
                            }
                            g_free(items);
                        }

                        if (!state->list_started) {
                            set_related_rows(state->global_bucket_labels[b], state->global_bucket_payloads[b]);
                            state->list_started = TRUE;
                        } else {
                            append_related_rows(state->global_bucket_labels[b], state->global_bucket_payloads[b]);
                        }

                        // clear bucket after flush without freeing strings
                        g_ptr_array_set_size(state->global_bucket_labels[b], 0);
                        g_ptr_array_set_size(state->global_bucket_payloads[b], 0);
                    }
                }
            }
        }

        if (!state->has_current_pos) {
            if (state->current_dict) {
                dict_entry_unref(state->current_dict);
                state->current_dict = NULL;
            }
            state->current_dict_count = 0;
        }

        g_free(word);
        if (clean_word) {
            g_free(clean_word);
        }
        g_free(word_key);
        processed++;

        if ((processed & 63) == 0) {
            if (g_get_monotonic_time() > deadline_us)
                break;
        }
    }

    return G_SOURCE_CONTINUE;
}

static guint seed_search_sidebar_fast_rows(SidebarSearchState *state) {
    if (!state || !state->query || !state->query_key) return 0;

    GPtrArray *labels = g_ptr_array_new();
    GPtrArray *payloads = g_ptr_array_new();
    const guint max_seed_rows = 512;
    gint64 deadline_us = g_get_monotonic_time() + 5000; /* 5ms UI thread budget */
    guint added = 0;

    for (guint idx = 0; state->search_entries && idx < state->search_entries->len && added < max_seed_rows; idx++) {
        if (g_get_monotonic_time() > deadline_us) break;
        DictEntry *entry = g_ptr_array_index(state->search_entries, idx);

        size_t pos = flat_index_search_prefix(entry->dict->index, state->query);
        while (pos != (size_t)-1 && added < max_seed_rows) {
            if ((pos & 63) == 0 && g_get_monotonic_time() > deadline_us) break;
            const FlatTreeEntry *node = flat_index_get(entry->dict->index, pos);
            if (!node) break;

            char *raw_word = g_strndup(entry->dict->data + node->h_off, node->h_len);
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
                    if (!g_hash_table_contains(state->seen_words, word_key)) {
                        char *display_word = normalize_headword_for_render(raw_word, node->h_len, TRUE);
                        RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                        payload->type = RELATED_ROW_CANDIDATE;
                        payload->word = g_strdup(raw_word);
                        payload->sort_key = g_utf8_casefold(display_word ? display_word : "", -1);
                        payload->fuzzy_score = score;

                        g_hash_table_add(state->seen_words, g_strdup(word_key));
                        g_ptr_array_add(labels, display_word);
                        g_ptr_array_add(payloads, payload);
                        added++;
                    }
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

    if (labels->len > 0) {
        set_related_rows(labels, payloads);
        state->list_started = TRUE;
        g_ptr_array_set_size(labels, 0);
        g_ptr_array_set_size(payloads, 0);
    }

    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);

    return added;
}

static void populate_search_sidebar_with_mode(const char *query, gboolean force_fts) {
    cancel_sidebar_search();

    char *clean = normalize_headword_for_search(query, FALSE);
    // If clean is NULL, it means the query is empty or whitespace-only.
    // We allow this to show all headwords.

    sidebar_search_state = g_new0(SidebarSearchState, 1);

    sidebar_search_state->is_fts = force_fts || (clean && g_str_has_prefix(clean, "* "));
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

    guint seeded = seed_search_sidebar_fast_rows(sidebar_search_state);
    if (sidebar_search_state->is_fts) {
        if (seeded == 0) {
            if (sidebar_search_state->fts_limited) {
                char *subtitle = g_strdup_printf("Searching the first %u of %u dictionaries in this scope.",
                                                 sidebar_search_state->searched_dict_count,
                                                 sidebar_search_state->scoped_dict_count);
                populate_search_sidebar_status("Full Text Search…", subtitle);
                g_free(subtitle);
            } else {
                populate_search_sidebar_status("Full Text Search…", NULL);
            }
        }
    } else if (seeded == 0) {
        populate_search_sidebar_status("Searching…", NULL);
    }
    sidebar_search_state->source_id = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE, continue_sidebar_search, sidebar_search_state, NULL);
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

    for (guint i = 0; i < favorite_words->len; i++) {
        const char *item = g_ptr_array_index(favorite_words, i);
        if (g_ascii_strcasecmp(item, clean) == 0) {
            if (!add) {
                g_ptr_array_remove_index(favorite_words, i);
            }
            save_word_list(favorite_words, FAVORITES_FILE_NAME);
            populate_favorites_sidebar();
            populate_history_sidebar();
        
            g_free(clean);
            return;
        }
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

    for (guint i = 0; i < history_words->len; i++) {
        const char *item = g_ptr_array_index(history_words, i);
        if (g_ascii_strcasecmp(item, clean) == 0) {
            g_ptr_array_remove_index(history_words, i);
            break;
        }
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
    if (!scope_id || g_strcmp0(scope_id, "all") == 0 || !app_settings) {
        return TRUE;
    }

    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        if (g_strcmp0(grp->id, scope_id) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static void ensure_valid_active_scope(void) {
    if (scope_id_exists(active_scope_id)) {
        return;
    }

    g_free(active_scope_id);
    active_scope_id = g_strdup("all");
}

static char *scope_display_name_dup(const char *scope_id) {
    if (!scope_id || g_strcmp0(scope_id, "all") == 0 || !app_settings) {
        return g_strdup("All Dictionaries");
    }

    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        if (g_strcmp0(grp->id, scope_id) == 0) {
            return g_strdup(grp->name ? grp->name : "Group");
        }
    }

    return g_strdup("All Dictionaries");
}

static guint count_enabled_dicts_in_scope(const char *scope_id) {
    guint count = 0;

    g_mutex_lock(&dict_loader_mutex);
    DictEntry *entry = all_dicts;
    while (entry) {
        dict_entry_ref(entry);
        g_mutex_unlock(&dict_loader_mutex);

        if (dict_entry_in_scope(entry, scope_id)) {
            count++;
        }

        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = entry->next;
        dict_entry_unref(entry);
        entry = next;
    }
    g_mutex_unlock(&dict_loader_mutex);

    return count;
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

            DictEntry *entry = g_hash_table_lookup(existing_by_path, cfg->path);
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
        SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
        payload->type = SIDEBAR_ROW_WORD;
        payload->title = g_strdup(word);
        g_ptr_array_add(labels, g_strdup(word));
        g_ptr_array_add(payloads, payload);
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
        SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
        payload->type = SIDEBAR_ROW_WORD;
        payload->title = g_strdup(word);
        g_ptr_array_add(labels, g_strdup(word));
        g_ptr_array_add(payloads, payload);
    }

    set_sidebar_list_rows(&favorites_sidebar, labels, payloads);
    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);
}

static void populate_groups_sidebar(void) {
    ensure_valid_active_scope();

    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *payloads = g_ptr_array_new();
    SidebarRowPayload *active_payload = NULL;

    SidebarRowPayload *all_payload = g_new0(SidebarRowPayload, 1);
    char all_subtitle[64];
    g_snprintf(all_subtitle, sizeof(all_subtitle), "%u dictionaries", count_enabled_dicts_in_scope("all"));
    all_payload->type = SIDEBAR_ROW_GROUP;
    all_payload->title = g_strdup("All Dictionaries");
    all_payload->subtitle = g_strdup(all_subtitle);
    all_payload->scope_id = g_strdup("all");
    g_ptr_array_add(labels, g_strdup(all_payload->title));
    g_ptr_array_add(payloads, all_payload);
    if (!active_scope_id || g_strcmp0(active_scope_id, "all") == 0) {
        active_payload = all_payload;
    }

    if (!app_settings) {
        set_sidebar_list_rows(&groups_sidebar, labels, payloads);
        sidebar_list_select_payload(&groups_sidebar, active_payload);
        g_ptr_array_free(labels, TRUE);
        g_ptr_array_free(payloads, TRUE);
        rebuild_search_scope_menu();
        update_search_scope_button_label();
        return;
    }

    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
        char subtitle[64];
        g_snprintf(subtitle, sizeof(subtitle), "%u dictionaries", count_enabled_dicts_in_scope(grp->id));
        payload->type = SIDEBAR_ROW_GROUP;
        payload->title = g_strdup(grp->name);
        payload->subtitle = g_strdup(subtitle);
        payload->scope_id = g_strdup(grp->id);
        g_ptr_array_add(labels, g_strdup(payload->title));
        g_ptr_array_add(payloads, payload);
        if (active_scope_id && g_strcmp0(active_scope_id, grp->id) == 0) {
            active_payload = payload;
        }
    }

    set_sidebar_list_rows(&groups_sidebar, labels, payloads);
    sidebar_list_select_payload(&groups_sidebar, active_payload);
    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);
    rebuild_search_scope_menu();
    update_search_scope_button_label();
}

static void update_search_scope_button_label(void) {
    if (!search_scope_button_label) {
        return;
    }

    ensure_valid_active_scope();
    char *label = scope_display_name_dup(active_scope_id);
    gtk_label_set_text(search_scope_button_label, label ? label : "All Dictionaries");
    gtk_widget_set_tooltip_text(GTK_WIDGET(search_scope_button), label ? label : "All Dictionaries");
    g_free(label);
}

static void rebuild_search_scope_menu(void) {
    if (!search_scope_button) {
        return;
    }

    ensure_valid_active_scope();

    GMenu *menu = g_menu_new();

    GMenuItem *all_item = g_menu_item_new("All Dictionaries", NULL);
    g_menu_item_set_action_and_target(all_item, "app.search-scope", "s", "all");
    g_menu_append_item(menu, all_item);
    g_object_unref(all_item);

    if (app_settings) {
        for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
            DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
            GMenuItem *item = g_menu_item_new(grp->name ? grp->name : "Group", NULL);
            g_menu_item_set_action_and_target(item, "app.search-scope", "s", grp->id);
            g_menu_append_item(menu, item);
            g_object_unref(item);
        }
    }

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
        populate_groups_sidebar();
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
        } else if (g_str_has_prefix(uri, "sound://")) {
            if (!try_play_encoded_sound_uri(uri)) {
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
                        play_audio_file(audio_path);
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
            play_audio_file(uri);
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
static void on_groups_item_activated(GtkListView *view, guint position, gpointer user_data);
static void on_dict_item_activated(GtkListView *view, guint position, gpointer user_data);

static void append_rendered_word_html_impl(const char *raw_word, gboolean push_history);
static void append_rendered_word_html(const char *raw_word);

static GtkWidget *create_sidebar_list_view(SidebarListView *sidebar, GCallback activate_cb) {
    sidebar->string_list = gtk_string_list_new(NULL);
    sidebar->payloads = g_ptr_array_new_with_free_func((GDestroyNotify)sidebar_row_payload_free);
    sidebar->selection_model = GTK_SINGLE_SELECTION(gtk_single_selection_new(G_LIST_MODEL(sidebar->string_list)));
    gtk_single_selection_set_autoselect(sidebar->selection_model, FALSE);
    gtk_single_selection_set_can_unselect(sidebar->selection_model, TRUE);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(sidebar_list_item_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(sidebar_list_item_bind), sidebar);

    sidebar->list_view = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(sidebar->selection_model), factory));
    gtk_list_view_set_single_click_activate(sidebar->list_view, TRUE);
    g_signal_connect(sidebar->list_view, "activate", activate_cb, sidebar);
    return GTK_WIDGET(sidebar->list_view);
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

static void related_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(box, "sidebar-row");
    gtk_widget_set_margin_start(box, 12);
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
    g_signal_connect_data(star_btn, "clicked", G_CALLBACK(on_sidebar_favorite_clicked), g_strdup(valid_text), free_signal_data, 0);
    
    gboolean is_fav = word_list_contains_ci(favorite_words, valid_text);
    gtk_button_set_icon_name(GTK_BUTTON(star_btn), is_fav ? "starred-symbolic" : "non-starred-symbolic");

    g_object_bind_property_full(item, "selected", star_btn, "visible", 
        G_BINDING_SYNC_CREATE,
        transform_sidebar_star_visibility, NULL, g_strdup(valid_text), g_free);

    g_free(valid_text);
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
    append_rendered_word_html(payload->word);
    if (related_selection_model) {
        gtk_single_selection_set_selected(related_selection_model, position);
    }
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
}



static void on_groups_item_activated(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    SidebarListView *sidebar = user_data;
    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    if (!payload || payload->type != SIDEBAR_ROW_GROUP) {
        return;
    }
    set_active_scope(payload->scope_id ? payload->scope_id : "all", TRUE);
    sidebar_list_select_payload(sidebar, payload);
}

static void on_dict_item_activated(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    SidebarListView *sidebar = user_data;
    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    if (!payload || payload->type != SIDEBAR_ROW_DICT || !payload->dict_entry) {
        return;
    }
    activate_dictionary_entry(payload->dict_entry);
    populate_dict_sidebar();
    sidebar_list_select_payload(sidebar, payload);
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
    const char *def_ptr = entry->dict->data + res->d_off;
    size_t def_len = res->d_len;

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
                def_ptr = entry->dict->data + red_res->d_off;
                def_len = red_res->d_len;
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

    return dsl_render_to_html(
        def_ptr, def_len,
        entry->dict->data + res->h_off, res->h_len,
        entry->format, entry->dict->resource_dir, entry->dict->source_dir, entry->dict->mdx_stylesheet, dark_mode,
        app_settings ? app_settings->color_theme : "default",
        render_style,
        app_settings ? app_settings->font_family : NULL,
        app_settings ? app_settings->font_size : 0,
        fts_highlight_query);
}

static void wrap_entry_in_style(GString *html_res, 
                               const char *headword, 
                               const char *dict_name, 
                               const char *icon_path,
                               const char *fallback_emoji,
                               const char *rendered_body, 
                               const char *render_style) {
    char *escaped_name = safe_markup_escape_n(dict_name ? dict_name : "", -1);
    char *escaped_headword = safe_markup_escape_n(headword, -1);
    char *icon_html = NULL;
    if (icon_path) {
        char *safe_icon = safe_markup_escape_n(icon_path, -1);
        icon_html = g_strdup_printf("<img src='file://%s' style='height:1.1em;vertical-align:middle;margin-right:0.35em;border-radius:2px;'> ", safe_icon);
        g_free(safe_icon);
    } else {
        icon_html = g_strdup_printf("%s ", fallback_emoji ? fallback_emoji : "📖");
    }

    if (g_strcmp0(render_style, "python") == 0) {
        g_string_append_printf(
            html_res,
            "<div class='entry'><div class='header'><div><span class='lemma'>%s</span></div>"
            "<span class='dict'>%s%s</span></div><div class='defs'>%s</div><hr></div>",
            escaped_headword, icon_html, escaped_name, rendered_body);
    } else if (g_strcmp0(render_style, "goldendict-ng") == 0) {
        g_string_append_printf(
            html_res,
            "<article class='gdarticle'><div class='gold-header'><span class='gold-entry-headword'>%s</span>"
            "<span class='gold-dict'>%s%s</span></div><div class='gdarticlebody'>%s</div></article>",
            escaped_headword, icon_html, escaped_name, rendered_body);
    } else if (g_strcmp0(render_style, "slate-card") == 0) {
        g_string_append_printf(
            html_res,
            "<section class='slate-entry'><div class='slate-header'><span class='slate-lemma'>%s</span>"
            "<span class='slate-dict'>%s%s</span></div><div class='slate-entry-body'>%s</div></section>",
            escaped_headword, icon_html, escaped_name, rendered_body);
    } else if (g_strcmp0(render_style, "paper") == 0) {
        g_string_append_printf(
            html_res,
            "<section class='paper-entry'><div class='paper-header'><span class='paper-lemma'>%s</span>"
            "<span class='paper-dict'>%s%s</span></div><div class='paper-entry-body'>%s</div></section>",
            escaped_headword, icon_html, escaped_name, rendered_body);
    } else {
        g_string_append_printf(
            html_res,
            "<section class='diction-entry'><div class='diction-header'><span class='diction-lemma'>%s</span>"
            "<span class='diction-dict'>%s%s</span></div><div class='diction-entry-body'>%s</div></section>",
            escaped_headword, icon_html, escaped_name, rendered_body);
    }

    g_free(icon_html);
    g_free(escaped_headword);
    g_free(escaped_name);
}

static void render_merged_group(GString *html_res,
                                DictEntry *e,
                                const char *headword,
                                GString *body_html,
                                int dict_idx,
                                int *dict_header_shown,
                                int *found_count) {
    if (body_html->len == 0) return;

    if (!*dict_header_shown) {
        *dict_header_shown = 1;
        e->has_matches = TRUE;
        (*found_count)++;
        g_string_append_printf(
            html_res,
            "<div id='dict-%d' class='dict-anchor' style='scroll-margin-top: 8px;'></div>",
            dict_idx);
    }

    const char *render_style = (app_settings && app_settings->render_style && *app_settings->render_style)
        ? app_settings->render_style : "diction";

    wrap_entry_in_style(html_res, headword, e->name, e->icon_path,
                        dict_format_emoji(e->format),
                        body_html->str, render_style);
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

static int append_exact_matches_html(GString *html_res, const char *query) {
    int found_count = 0;
    int dict_idx = 0;

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

        // ... (rest of search logic)

        size_t pos = flat_index_search(e->dict->index, query);
        int dict_header_shown = 0;
        char *last_hw_clean = NULL;
        char *last_hw_render = NULL;
        GString *merged_body = g_string_new("");

        while (pos != (size_t)-1) {
            const FlatTreeEntry *res = flat_index_get(e->dict->index, pos);
            if (!res) break;
            if (compare_headword(e->dict->data, res, query, strlen(query)) != 0) break;

            char *raw_hw = g_strndup(e->dict->data + res->h_off, res->h_len);
            char *clean_hw = normalize_headword_for_search(raw_hw, TRUE);
            char *display_hw = normalize_headword_for_render(raw_hw, strlen(raw_hw), TRUE);
            const char *hw_to_use = clean_hw ? clean_hw : raw_hw;
            const char *hw_to_render = display_hw ? display_hw : hw_to_use;

            if (last_hw_clean && strcmp(hw_to_use, last_hw_clean) != 0) {
                render_merged_group(html_res, e, last_hw_render, merged_body, dict_idx, &dict_header_shown, &found_count);
                g_string_truncate(merged_body, 0);
                g_clear_pointer(&last_hw_render, g_free);
            }

            char *rendered = render_entry_def_to_html(e, res);
            if (rendered) {
                if (merged_body->len > 0) {
                    g_string_append(merged_body, "<hr style='border:none;border-top:1px dashed #ccc;margin:10px 0;opacity:0.5;'>");
                }
                g_string_append(merged_body, rendered);
                g_free(rendered);
            }

            if (!last_hw_render || (strstr(hw_to_render, "\xC2\xB7") && !strstr(last_hw_render, "\xC2\xB7"))) {
                g_free(last_hw_render);
                last_hw_render = g_strdup(hw_to_render);
            }

            g_free(last_hw_clean);
            last_hw_clean = g_strdup(hw_to_use);
            g_free(raw_hw);
            if (clean_hw) g_free(clean_hw);
            if (display_hw) g_free(display_hw);

            pos++;
            if (pos >= flat_index_count(e->dict->index)) break;
        }

        if (last_hw_render && merged_body->len > 0) {
            render_merged_group(html_res, e, last_hw_render, merged_body, dict_idx, &dict_header_shown, &found_count);
        }
        g_string_free(merged_body, TRUE);
        g_free(last_hw_clean);
        g_free(last_hw_render);
        dict_idx++;
        
        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = e->next;
        dict_entry_unref(e);
        e = next;
    }
    if (e == NULL) g_mutex_unlock(&dict_loader_mutex);

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

    GString *html_res = g_string_new("<html><body>");
    char *escaped_query_attr = safe_markup_escape_n(query, -1);
    g_string_append_printf(html_res, "<div class='word-group' data-word='%s'>", escaped_query_attr);
    g_free(escaped_query_attr);
    gsize html_prefix_len = html_res->len;

    g_mutex_lock(&dict_loader_mutex);
    for (DictEntry *e = all_dicts; e; e = e->next) e->has_matches = FALSE;
    g_mutex_unlock(&dict_loader_mutex);

    int found_count = append_exact_matches_html(html_res, query);
    if (found_count == 0) {
        char *fallback_query = exact_lookup_definite_article_variant(query);
        if (fallback_query) {
            g_string_truncate(html_res, html_prefix_len);
            found_count = append_exact_matches_html(html_res, fallback_query);
            g_free(fallback_query);
        }
    }

    if (found_count > 0) {
        queue_fts_highlight_for_web_view(target_wv,
                                         should_highlight_fts ? fts_highlight_query : NULL);
        g_string_append(html_res, "</div></body></html>");
        webkit_web_view_load_html(target_wv, html_res->str, "file:///");
        set_tab_metadata(target_wv, query, query, 1);
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

    char *display_title = normalize_headword_for_render(raw_word, raw_word ? strlen(raw_word) : 0, TRUE);

    GString *html_res = g_string_new("");
    int found_count = append_exact_matches_html(html_res, query);
    if (found_count == 0) {
        char *fallback_query = exact_lookup_definite_article_variant(query);
        if (fallback_query) {
            g_string_truncate(html_res, 0);
            found_count = append_exact_matches_html(html_res, fallback_query);
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

        char *b64_html = g_base64_encode((const guchar *)html_res->str, html_res->len);
        char *b64_word = g_base64_encode((const guchar *)query, strlen(query));

        WebKitWebView *wv = get_current_web_view();
        if (wv) {
            queue_fts_highlight_for_web_view(wv,
                                             should_highlight_fts ? fts_highlight_query : NULL);
            webkit_web_view_load_html(wv, html_res->str, "file:///");
        }

        char *js = g_strdup_printf(
            "var decWord = atob('%s');"
            "var bytesWord = new Uint8Array(decWord.length);"
            "for(var i=0; i<decWord.length; i++) bytesWord[i] = decWord.charCodeAt(i);"
            "var word = new TextDecoder('utf-8').decode(bytesWord);"

            "var b64Html = '%s';"

            "var existing = document.querySelector(\".word-group[data-word='\" + CSS.escape(word) + \"']\");"
            "if (existing) {"
            "    existing.scrollIntoView({behavior: 'smooth', block: 'start'});"
            "} else {"
            "    var wrapper = document.createElement('div');"
            "    wrapper.className = 'word-group';"
            "    wrapper.setAttribute('data-word', word);"
            "    var dec = atob(b64Html);"
            "    var bytes = new Uint8Array(dec.length);"
            "    for (var i = 0; i < dec.length; i++) bytes[i] = dec.charCodeAt(i);"
            "    wrapper.innerHTML = new TextDecoder('utf-8').decode(bytes);"
            "    document.body.insertBefore(wrapper, document.body.firstChild);"
            "    wrapper.scrollIntoView({behavior: 'smooth', block: 'start'});"
            "}",
            b64_word, b64_html);
            
        webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);

        g_free(js);
        g_free(b64_word);
        g_free(b64_html);
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
    char *display_query = normalize_headword_for_render(query, query ? strlen(query) : 0, TRUE);

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
            const char *raw_data = target_e->dict->data + node->h_off;
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
    if (!e) return;
    int idx = -1;
    int current = 0;
    g_mutex_lock(&dict_loader_mutex);
    DictEntry *cursor = all_dicts;
    while (cursor) {
        dict_entry_ref(cursor);
        g_mutex_unlock(&dict_loader_mutex);

        if (cursor->dict && dict_entry_in_active_scope(cursor)) {
            if (cursor == e) {
                idx = current;
                dict_entry_unref(cursor);
                break;
            }
            current++;
        }
        
        g_mutex_lock(&dict_loader_mutex);
        DictEntry *next = cursor->next;
        dict_entry_unref(cursor);
        cursor = next;
    }
    if (cursor == NULL) g_mutex_unlock(&dict_loader_mutex);
    if (idx < 0) {
        return;
    }

    char js[256];
    snprintf(js, sizeof(js),
        "var el = document.getElementById('dict-%d'); "
        "if (el) { el.scrollIntoView({behavior: 'smooth', block: 'start'}); }",
        idx);
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
    populate_groups_sidebar();
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

    /* Update all WebKit views to match palette */
    GdkRGBA bg_color;
    if (!gdk_rgba_parse(&bg_color, palette.bg))
        gdk_rgba_parse(&bg_color, dark_mode ? "#1e1e21" : "#ffffff");

    if (tab_view) {
        GListModel *pages = G_LIST_MODEL(adw_tab_view_get_pages(tab_view));
        guint n_pages = g_list_model_get_n_items(pages);
        for (guint i = 0; i < n_pages; i++) {
            AdwTabPage *page = ADW_TAB_PAGE(g_list_model_get_item(pages, i));
            GtkWidget *scroll = adw_tab_page_get_child(page);
            WebKitWebView *wv = get_web_view_from_scroll(scroll);
            if (wv) {
                webkit_web_view_set_background_color(wv, &bg_color);
            }
        }
    }

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

    /* Selection colors: standard blue for default, palette accent for others */
    char sidebar_select[64];
    if (is_default_theme) {
        g_strlcpy(sidebar_select, dark_mode ? "rgba(255, 255, 255, 0.1)" : "rgba(0, 0, 0, 0.08)", sizeof(sidebar_select));
    } else {
        g_snprintf(sidebar_select, sizeof(sidebar_select), "rgba(%u,%u,%u,0.25)", ar, ag, ab);
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
        "row:selected, listitem:selected {\n"
        "  background-color: %s;\n"
        "  color: inherit;\n"
        "}\n"
        "/* Explicitly set webview backgrounds */\n"
        "webkitwebview, webview {\n"
        "  background-color: @view_bg_color;\n"
        "}\n"
        "row:hover:not(:selected), listitem:hover:not(:selected) {\n"
        "  background-color: %s;\n"
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
        "  background-color: %s;\n"
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
        "}\n",
        /* @define-color values */
        w_bg, w_fg, ch_bg, w_fg, h_bg, w_fg,
        sidebar_bg, w_fg, popover_bg, w_fg, accent,
        /* sidebar border (1) */
        palette.border,
        /* row:selected (1) */
        sidebar_select,
        /* row:hover (1) */
        hover_color,
        /* popover row hover (1) */
        hover_color,
        /* content header (1) */
        ch_bg,
        /* tab bar (1) */
        ch_bg
    );

    gtk_css_provider_load_from_string(dynamic_theme_provider, css);
    g_free(css);

    refresh_search_results();
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
    clear_sidebar_list(&groups_sidebar);
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
    populate_groups_sidebar();
    populate_search_sidebar(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    startup_loading_active = TRUE;
    if (!start_async_dict_loading(discover_from_dirs)) {
        startup_loading_active = FALSE;
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
    extern void settings_scan_notify(const char *name, const char *path, int event_type);
    settings_scan_notify(NULL, NULL, -1);
    if (!active_entry && all_dicts) {
        set_active_entry(all_dicts);
    }
    populate_dict_sidebar();
    populate_groups_sidebar();
    populate_history_sidebar();
    populate_favorites_sidebar();
    populate_search_sidebar(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    refresh_search_results();

    if (startup_loading_active) {
        close_startup_splash();
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
    populate_groups_sidebar();
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
    SidebarRowPayload *active_payload = NULL;

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
            dict_entry_ref(e); // payload owns a ref
            if (e->icon_path) {
                payload->icon_path = g_strdup(e->icon_path);
            }
            g_ptr_array_add(labels, g_strdup(payload->title));
            g_ptr_array_add(payloads, payload);
            if (e == active_entry) {
                active_payload = payload;
            }
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
    if (active_payload) {
        sidebar_list_select_payload(&dict_sidebar, active_payload);
    } else if (dict_sidebar.selection_model) {
        gtk_single_selection_set_selected(dict_sidebar.selection_model, GTK_INVALID_LIST_POSITION);
    }

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
    g_idle_add(on_dict_loaded_idle, ld);
}

static char* sample_dict_and_detect_lang(DictEntry *entry) {
    if (!entry || !entry->dict || !entry->dict->index) return NULL;
    
    size_t total = flat_index_count(entry->dict->index);
    if (total == 0) return NULL;

    GString *hw_samples = g_string_new("");
    GString *def_samples = g_string_new("");

    int count = total < 50 ? (int)total : 50;
    for (int i = 0; i < count; i++) {
        size_t idx = rand() % total;
        const FlatTreeEntry *node = flat_index_get(entry->dict->index, idx);
        if (!node) continue;

        char *hw = g_strndup(entry->dict->data + node->h_off, node->h_len);
        char *def = g_strndup(entry->dict->data + node->d_off, node->d_len);
        
        g_string_append_printf(hw_samples, " %s ", hw);
        g_string_append_printf(def_samples, " %s ", def);
        
        g_free(hw);
        g_free(def);
    }

    const char *hw_lang = langid_guess_language(hw_samples->str);
    const char *def_lang = langid_guess_language(def_samples->str);

    g_string_free(hw_samples, TRUE);
    g_string_free(def_samples, TRUE);

    if (g_strcmp0(hw_lang, "Unknown") == 0 && g_strcmp0(def_lang, "Unknown") == 0) {
        return g_strdup("Mixed");
    }

    if (g_strcmp0(def_lang, "Unknown") == 0) def_lang = hw_lang;
    if (g_strcmp0(hw_lang, "Unknown") == 0) hw_lang = def_lang;

    char *group = langpair_build_group_name(hw_lang, def_lang);
    if (group) {
        return group;
    }

    return g_strdup_printf("%s->%s", hw_lang, def_lang);
}

static gboolean lang_group_is_monolingual(const char *group_name) {
    if (!group_name) {
        return FALSE;
    }

    const char *sep = strstr(group_name, "->");
    if (!sep) {
        return FALSE;
    }

    char *left = g_strndup(group_name, (gsize)(sep - group_name));
    char *right = g_strdup(sep + 2);
    gboolean same = g_strcmp0(left, right) == 0;
    g_free(left);
    g_free(right);
    return same;
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

    update_startup_splash_progress(ld->completed, ld->total, ld->status_text);

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
            if (curr->path && strcmp(curr->path, e->path) == 0) {
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
            gboolean parser_had_langs =
                (e->dict->source_lang && *e->dict->source_lang) ||
                (e->dict->target_lang && *e->dict->target_lang);

            langpair_fill_missing(&source_lang, &target_lang, metadata_text, e->path);
            guessed = langpair_build_group_name(source_lang, target_lang);

            if (!parser_had_langs && (!guessed || lang_group_is_monolingual(guessed))) {
                /* No headers found, or only a weak/monolingual guess from filename.
                 * Perform deep sampling to improve language identification. */
                char *sampled = sample_dict_and_detect_lang(e);
                if (sampled && *sampled && g_strcmp0(sampled, "Mixed") != 0) {
                    g_free(guessed);
                    guessed = sampled;
                    sampled = NULL;
                }
                g_free(sampled);
            }

            /* Fallback only if we still have absolutely nothing */
            if (!guessed && !parser_had_langs) {
                guessed = sample_dict_and_detect_lang(e);
            }

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
            if (curr->path && strcmp(curr->path, e->path) == 0) {
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
        if (!startup_loading_active && (ld->completed % 50 == 0)) {
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

static void dbus_global_shortcut_activated(GDBusConnection *connection,
                                           const gchar *sender_name,
                                           const gchar *object_path,
                                           const gchar *interface_name,
                                           const gchar *signal_name,
                                           GVariant *parameters,
                                           gpointer user_data) {
    (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name; (void)user_data;

    const char *session_handle = NULL;
    const char *shortcut_id = NULL;
    guint64 timestamp = 0;
    GVariant *options = NULL;
    g_variant_get(parameters, "(&o&st@a{sv})", &session_handle, &shortcut_id, &timestamp, &options);
    (void)timestamp;

    if (g_strcmp0(session_handle, global_shortcut_session_handle) == 0 &&
        g_strcmp0(shortcut_id, GLOBAL_SHORTCUT_ID) == 0) {
        
        if (options) {
            const char *token = NULL;
            if (g_variant_lookup(options, "activation_token", "&s", &token) && token) {
                g_free(pending_activation_token);
                pending_activation_token = g_strdup(token);
                fprintf(stderr, "[Shortcut] Got token: %s\n", token);
            }
        }
        
        fprintf(stderr, "[Shortcut] Triggering scan popup manual fetch!\n");
        scan_popup_trigger_manual();
    } else {
        fprintf(stderr, "[Shortcut] Ignoring unmatched ID: %s\n", shortcut_id ? shortcut_id : "null");
    }

    if (options) {
        g_variant_unref(options);
    }
}

static char *portal_sender_path_component(GDBusConnection *conn) {
    const char *unique_name = g_dbus_connection_get_unique_name(conn);
    if (!unique_name) {
        return NULL;
    }

    char *component = g_strdup(unique_name[0] == ':' ? unique_name + 1 : unique_name);
    g_strdelimit(component, ".", '_');
    return component;
}

static char *portal_token(const char *prefix) {
    return g_strdup_printf("diction_%s_%u", prefix, g_random_int());
}

static char *portal_request_path_for_token(GDBusConnection *conn, const char *token) {
    char *sender = portal_sender_path_component(conn);
    if (!sender) {
        return NULL;
    }

    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    g_free(sender);
    return path;
}

static char *portal_session_path_for_token(GDBusConnection *conn, const char *token) {
    char *sender = portal_sender_path_component(conn);
    if (!sender) {
        return NULL;
    }

    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/session/%s/%s", sender, token);
    g_free(sender);
    return path;
}

static void global_shortcut_request_response(GDBusConnection *connection,
                                             const gchar *sender_name,
                                             const gchar *object_path,
                                             const gchar *interface_name,
                                             const gchar *signal_name,
                                             GVariant *parameters,
                                             gpointer user_data);

static void global_shortcut_call_done(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    const char *method = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), res, &err);
    if (err) {
        g_warning("Global shortcut portal %s call failed: %s", method ? method : "unknown", err->message);
        g_error_free(err);
        return;
    }

    if (reply) {
        g_variant_unref(reply);
    }
}

static void bind_global_shortcut(void) {
    if (!global_shortcut_conn || !global_shortcut_session_handle) {
        return;
    }

    char *request_token = portal_token("bind");
    char *request_path = portal_request_path_for_token(global_shortcut_conn, request_token);
    if (!request_path) {
        g_free(request_token);
        return;
    }

    if (global_shortcut_bind_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_bind_response_sub_id);
        global_shortcut_bind_response_sub_id = 0;
    }
    global_shortcut_bind_response_sub_id =
        g_dbus_connection_signal_subscribe(global_shortcut_conn,
                                           "org.freedesktop.portal.Desktop",
                                           "org.freedesktop.portal.Request",
                                           "Response",
                                           request_path,
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           global_shortcut_request_response,
                                           GINT_TO_POINTER(PORTAL_REQUEST_BIND_SHORTCUTS),
                                           NULL);

    GVariantBuilder shortcut_props;
    g_variant_builder_init(&shortcut_props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&shortcut_props, "{sv}", "description",
                          g_variant_new_string("Scan selected text with Diction"));
    g_variant_builder_add(&shortcut_props, "{sv}", "preferred_trigger",
                          g_variant_new_string(GLOBAL_SHORTCUT_TRIGGER));

    GVariantBuilder shortcuts;
    g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
    g_variant_builder_add(&shortcuts, "(sa{sv})", GLOBAL_SHORTCUT_ID, &shortcut_props);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token));

    g_dbus_connection_call(global_shortcut_conn,
                           "org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.GlobalShortcuts",
                           "BindShortcuts",
                           g_variant_new("(oa(sa{sv})sa{sv})",
                                         global_shortcut_session_handle,
                                         &shortcuts,
                                         "",
                                         &options),
                           G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           global_shortcut_call_done,
                           "BindShortcuts");

    g_free(request_path);
    g_free(request_token);
}

static void global_shortcut_request_response(GDBusConnection *connection,
                                             const gchar *sender_name,
                                             const gchar *object_path,
                                             const gchar *interface_name,
                                             const gchar *signal_name,
                                             GVariant *parameters,
                                             gpointer user_data) {
    (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;

    PortalRequestKind kind = GPOINTER_TO_INT(user_data);
    guint response = 2;
    GVariant *results = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if (kind == PORTAL_REQUEST_CREATE_SHORTCUT_SESSION &&
        global_shortcut_create_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(connection, global_shortcut_create_response_sub_id);
        global_shortcut_create_response_sub_id = 0;
    } else if (kind == PORTAL_REQUEST_BIND_SHORTCUTS &&
               global_shortcut_bind_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(connection, global_shortcut_bind_response_sub_id);
        global_shortcut_bind_response_sub_id = 0;
    }

    if (response != 0) {
        g_warning("Global shortcut portal request failed or was cancelled (response %u)", response);
        if (results) {
            g_variant_unref(results);
        }
        return;
    }

    if (kind == PORTAL_REQUEST_CREATE_SHORTCUT_SESSION) {
        const char *session_handle = NULL;
        if (!results || !g_variant_lookup(results, "session_handle", "&s", &session_handle)) {
            g_warning("Global shortcut portal did not return a session_handle");
        } else {
            g_free(global_shortcut_session_handle);
            global_shortcut_session_handle = g_strdup(session_handle);

            if (global_shortcut_signal_sub_id == 0) {
                global_shortcut_signal_sub_id =
                    g_dbus_connection_signal_subscribe(global_shortcut_conn,
                                                       "org.freedesktop.portal.Desktop",
                                                       "org.freedesktop.portal.GlobalShortcuts",
                                                       "Activated",
                                                       "/org/freedesktop/portal/desktop",
                                                       global_shortcut_session_handle,
                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                       dbus_global_shortcut_activated,
                                                       NULL,
                                                       NULL);
            }

            bind_global_shortcut();
        }
    }

    if (results) {
        g_variant_unref(results);
    }
}

static void setup_global_shortcut(void) {
    if (global_shortcut_conn) {
        return;
    }

    global_shortcut_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!global_shortcut_conn) {
        return;
    }

    char *request_token = portal_token("create");
    char *session_token = portal_token("session");
    char *request_path = portal_request_path_for_token(global_shortcut_conn, request_token);
    char *session_path = portal_session_path_for_token(global_shortcut_conn, session_token);
    if (!request_path || !session_path) {
        g_free(request_path);
        g_free(session_path);
        g_free(request_token);
        g_free(session_token);
        return;
    }

    global_shortcut_create_response_sub_id =
        g_dbus_connection_signal_subscribe(global_shortcut_conn,
                                           "org.freedesktop.portal.Desktop",
                                           "org.freedesktop.portal.Request",
                                           "Response",
                                           request_path,
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           global_shortcut_request_response,
                                           GINT_TO_POINTER(PORTAL_REQUEST_CREATE_SHORTCUT_SESSION),
                                           NULL);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token));
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(session_token));

    g_dbus_connection_call(global_shortcut_conn,
                           "org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.GlobalShortcuts",
                           "CreateSession",
                           g_variant_new("(a{sv})", &options),
                           G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           global_shortcut_call_done,
                           "CreateSession");

    g_free(request_path);
    g_free(session_path);
    g_free(request_token);
    g_free(session_token);
}

static void destroy_global_shortcut(void) {
    if (!global_shortcut_conn) {
        g_clear_pointer(&global_shortcut_session_handle, g_free);
        return;
    }

    if (global_shortcut_create_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_create_response_sub_id);
        global_shortcut_create_response_sub_id = 0;
    }
    if (global_shortcut_bind_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_bind_response_sub_id);
        global_shortcut_bind_response_sub_id = 0;
    }
    if (global_shortcut_signal_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_signal_sub_id);
        global_shortcut_signal_sub_id = 0;
    }

    if (global_shortcut_session_handle) {
        g_dbus_connection_call(global_shortcut_conn,
                               "org.freedesktop.portal.Desktop",
                               global_shortcut_session_handle,
                               "org.freedesktop.portal.Session",
                               "Close",
                               NULL,
                               NULL,
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               NULL,
                               NULL,
                               NULL);
    }

    g_clear_pointer(&global_shortcut_session_handle, g_free);
    g_clear_object(&global_shortcut_conn);
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
    gtk_single_selection_set_can_unselect(related_selection_model, TRUE);

    GtkListItemFactory *related_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(related_factory, "setup", G_CALLBACK(related_list_item_setup), NULL);
    g_signal_connect(related_factory, "bind", G_CALLBACK(related_list_item_bind), NULL);

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

    /* Groups Tab */
    GtkWidget *groups_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(groups_scroll, TRUE);
    gtk_widget_set_hexpand(groups_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(groups_scroll),
        create_sidebar_list_view(&groups_sidebar, G_CALLBACK(on_groups_item_activated)));
    adw_view_stack_add_titled_with_icon(sidebar_stack, groups_scroll, "groups", "Groups", "folder-symbolic");

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
        {"folder-symbolic", "groups", "Groups"},
        {"accessories-dictionary-symbolic", "dictionaries", "Dictionaries"}
    };

    GtkWidget *first_btn = NULL;
    for (int i = 0; i < 5; i++) {
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
    GtkWidget *scope_icon = gtk_image_new_from_icon_name("folder-symbolic");
    search_scope_button_label = GTK_LABEL(gtk_label_new("All Dictionaries"));
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
    populate_groups_sidebar();
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
        ".sidebar-tabs button { padding-left: 12px; padding-right: 12px; padding-top: 8px; padding-bottom: 8px; margin-left:  0.5px; margin-right: 0.5px; min-height: 0; min-width: 0; border: none; border-radius: 10px; }"
        ".sidebar-tabs button image { opacity: 0.7; }"
        ".sidebar-tabs button:checked { background: alpha(@theme_fg_color, 0.1); }"
        ".sidebar-tabs button:checked image { opacity: 1.0; }"
        ".sidebar-row { min-height: 34px; }"
        ".navigation-sidebar { background: transparent; }"
        ".navigation-sidebar listitem:hover, .navigation-sidebar row:hover { background: alpha(@theme_fg_color, 0.05); }"
        ".navigation-sidebar listitem:selected, .navigation-sidebar row:selected { background: alpha(@theme_fg_color, 0.1); color: inherit; }"
        ".content-header { background: transparent; }\n"
        ".menu-item { font-weight: normal; padding: 4px 8px; min-height: 0; }"
        "overlay-split-view > separator { background: @sidebar_bg_color; min-width: 1px; opacity: 1; }"
        "headerbar.sidebar { box-shadow: none; border-bottom: none; margin: 0; padding: 0; }"
        ".search-button-bg { background: alpha(@theme_fg_color, 0.08); border-radius: 6px 0 0 6px; padding: 0px 8px; }"
        ".search-button-bg:hover { background: alpha(@theme_fg_color, 0.12); }"
        ".search-scope-button { background: alpha(@theme_fg_color, 0.08); border-radius: 0 6px 6px 0; padding: 0px 10px; margin-left: 0; }"
        ".search-scope-button:hover { background: alpha(@theme_fg_color, 0.12); }"
        "entry.search-entry-joined { border-top-right-radius: 0; border-bottom-right-radius: 0; }"
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

    render_idle_page_to_webview(web_view, "Loading dictionaries...", "Please wait.");

    // Start async loading if we have settings-based dirs
    if (!all_dicts || had_cached_entries) {
        startup_loading_active = TRUE;
        startup_random_word_pending = (!search_entry ||
                                       strlen(gtk_editable_get_text(GTK_EDITABLE(search_entry))) == 0);
        if (start_async_dict_loading(discover_from_dirs)) {
            if (had_cached_entries && !discover_from_dirs) {
                gtk_window_present(GTK_WINDOW(window));
            } else {
                show_startup_splash(app);
            }
        } else {
            startup_loading_active = FALSE;
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

    setup_global_shortcut();
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

int main(int argc, char *argv[]) {
    // Disable compositing to fix rendering issues
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
    // Seed random number generator
    srand(time(NULL));
    // Load settings first
    app_settings = settings_load();
    active_scope_id = g_strdup("all");
    history_words = load_word_list(HISTORY_FILE_NAME, 200);
    favorite_words = load_word_list(FAVORITES_FILE_NAME, 0);

    /* Load dictionaries only in CLI mode (single file or directory argument).      *
     * When running with no arguments, loading happens async after window is shown. */
    if (argc > 1) {
        struct stat st;
        if (stat(argv[1], &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                all_dicts = dict_loader_scan_directory(argv[1]);
            } else {
                /* Single file mode */
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
    destroy_global_shortcut();
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
