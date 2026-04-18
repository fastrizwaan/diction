#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <adwaita.h>
#include "langid.h"
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

static WebKitWebView *get_current_web_view(void) {
    if (!tab_view) return NULL;
    AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
    if (!page) return NULL;
    GtkWidget *scroll = adw_tab_page_get_child(page);
    if (!scroll || !GTK_IS_SCROLLED_WINDOW(scroll)) return NULL;
    GtkWidget *child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
    if (GTK_IS_VIEWPORT(child)) {
        child = gtk_viewport_get_child(GTK_VIEWPORT(child));
    }
    return WEBKIT_WEB_VIEW(child);
}
#define web_view get_current_web_view()

static AdwStyleManager *style_manager = NULL;
static GtkSearchEntry *search_entry = NULL;
static GtkStack *search_stack = NULL;

static AdwTabPage *create_new_tab(const char *title, gboolean select_it);
static void on_tab_selected(AdwTabView *view, GParamSpec *pspec, gpointer user_data);
static GtkButton *search_button = NULL;
static GtkLabel *search_button_label = NULL;
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
static guint search_execute_source_id = 0;
static GtkStringList *related_string_list = NULL;
static GtkSingleSelection *related_selection_model = NULL;
static GtkListView *related_list_view = NULL;
static GPtrArray *related_row_payloads = NULL;
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
        "  border-radius: 24px;"
        "  padding: 22px 24px 18px 24px;"
        "  border: 1px solid alpha(@accent_bg_color, 0.18);"
        "  background: linear-gradient(135deg, alpha(@accent_bg_color, 0.10), alpha(@window_bg_color, 0.98));"
        "}"
        ".startup-logo-wrap {"
        "  min-width: 72px;"
        "  min-height: 72px;"
        "  border-radius: 18px;"
        "  background: alpha(@accent_bg_color, 0.14);"
        "}"
        ".startup-title {"
        "  font-size: 1.55rem;"
        "  font-weight: 800;"
        "  letter-spacing: -0.02em;"
        "}"
        ".startup-subtitle {"
        "  opacity: 0.78;"
        "  font-size: 0.96rem;"
        "}"
        ".startup-meta {"
        "  min-height: 20px;"
        "}"
        ".startup-status {"
        "  opacity: 0.82;"
        "  font-size: 0.92rem;"
        "}"
        ".startup-count {"
        "  opacity: 0.62;"
        "  font-size: 0.84rem;"
        "  font-variant-numeric: tabular-nums;"
        "}"
        ".startup-progress trough, .startup-progress progress {"
        "  min-height: 7px;"
        "  border-radius: 999px;"
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
    gtk_window_set_default_size(window, 480, 168);
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

    GtkWidget *copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(copy, TRUE);
    gtk_box_append(GTK_BOX(header), copy);

    GtkWidget *title = gtk_label_new("Diction");
    gtk_widget_add_css_class(title, "startup-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(copy), title);

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




#define HISTORY_FILE_NAME "history.json"
#define FAVORITES_FILE_NAME "favorites.json"
static void populate_dict_sidebar(void);      // forward declaration
static gboolean start_async_dict_loading(gboolean discover_from_dirs);   // forward declaration
static void on_search_changed(GtkSearchEntry *entry, gpointer user_data); // forward declaration
static void on_random_clicked(GtkButton *btn, gpointer user_data);
static void maybe_show_startup_random_word(void);
static void refresh_search_results(void);
static void populate_search_sidebar(const char *query);
static void execute_search_now(void);
static void activate_dictionary_entry(DictEntry *e);
static void finalize_dictionary_loading(gboolean allow_random_word, gboolean sync_settings_from_loaded);
static gboolean on_dict_loaded_idle(gpointer user_data);

#define BUCKET_COUNT 6

typedef struct {
    char *query;
    char *query_key;
    guint query_len;
    GHashTable *seen_words;
    DictEntry *current_entry;
    DictEntry *current_dict;
    size_t current_pos;  /* position in flat index */
    gboolean has_current_pos;
    gboolean list_started;
    guint source_id;
    GPtrArray *global_bucket_labels[BUCKET_COUNT];
    GPtrArray *global_bucket_payloads[BUCKET_COUNT];
} SidebarSearchState;

static SidebarSearchState *sidebar_search_state = NULL;

typedef enum {
    RELATED_ROW_HINT = 0,
    RELATED_ROW_CANDIDATE
} RelatedRowType;

typedef struct {
    RelatedRowType type;
    char *word;
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
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (e->dict && e->dict->resource_dir && g_strcmp0(e->dict->resource_dir, resource_dir) == 0) {
            if (e->dict->resource_reader) {
                fprintf(stderr, "[AUDIO DEBUG] Searching ResourceReader for '%s'\n", sound_file);
                audio_path = resource_reader_get(e->dict->resource_reader, sound_file);
                if (audio_path) break;
            }
        }
    }

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

static char *normalize_headword_for_search(const char *value, gboolean unescape_dsl) {
    if (!value) {
        return NULL;
    }

    char *valid = g_utf8_make_valid(value, -1);
    GString *out = g_string_new("");
    const char *p = valid;

    while (*p) {
        if (g_str_has_prefix(p, "{*}")) {
            p += 3;
            continue;
        }

        if (g_str_has_prefix(p, "{·}")) {
            p += 3;
            continue;
        }

        /* Strip stress accent tags for search */
        if (g_str_has_prefix(p, "{[']}")) { p += 5; continue; }
        if (g_str_has_prefix(p, "{[/']}")) { p += 6; continue; }
        if (g_str_has_prefix(p, "[']")) { p += 3; continue; }
        if (g_str_has_prefix(p, "[/']")) { p += 4; continue; }

        /* Strip actual Unicode combining acute accent (U+0301) for search */
        if (g_str_has_prefix(p, "\xCC\x81")) { p += 2; continue; }
        
        /* Preserve bare braces unless they are part of a backslash escape */
        if (*p == '{' || *p == '}') {
            g_string_append_c(out, *p);
            p++;
            continue;
        }

        if (*p == '\\' && p[1] != '\0') {
            if (unescape_dsl) {
                /* DSL mandatory special characters that MUST be unescaped:
                 * { } \ ~ @ # ( ) [ ] < >. Any other character (like / in \/) 
                 * is NOT unescaped to preserve leet speak patterns. */
                const char *special = " {}~\\@#()[]<>";
                if (strchr(special, p[1])) {
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
    g_free(payload);
}

static void sidebar_row_payload_free(SidebarRowPayload *payload) {
    if (!payload) {
        return;
    }
    g_free(payload->title);
    g_free(payload->subtitle);
    g_free(payload->scope_id);
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
    if (state->seen_words) {
        g_hash_table_unref(state->seen_words);
    }
    for (int i = 0; i < BUCKET_COUNT; i++) {
        if (state->global_bucket_labels[i])
            g_ptr_array_free(state->global_bucket_labels[i], TRUE);
        if (state->global_bucket_payloads[i]) {
            g_ptr_array_free(state->global_bucket_payloads[i], TRUE);
        }
    }
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
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
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

static void sidebar_list_item_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = sidebar_list_item_make_label();
    gtk_widget_set_hexpand(label, TRUE);
    
    GtkWidget *star_btn = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_add_css_class(star_btn, "flat");
    gtk_widget_set_valign(star_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(star_btn, 4);

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), star_btn);
    gtk_list_item_set_child(item, box);
}

static void sidebar_list_item_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data) {
    (void)factory;
    SidebarListView *sidebar = user_data;
    GtkWidget *box = gtk_list_item_get_child(item);
    GtkWidget *label = gtk_widget_get_first_child(box);
    GtkWidget *star_btn = gtk_widget_get_last_child(box);

    guint position = gtk_list_item_get_position(item);
    SidebarRowPayload *payload = sidebar_payload_at(sidebar, position);
    const char *title = payload && payload->title ? payload->title : "";
    const char *subtitle = payload && payload->subtitle ? payload->subtitle : "";
    char *safe_title = safe_markup_escape_n(title, -1);
    char *safe_subtitle = safe_markup_escape_n(subtitle, -1);
    char *markup = NULL;

    if (payload && payload->type == SIDEBAR_ROW_HINT) {
        if (*safe_subtitle) {
            markup = g_strdup_printf("<span alpha='75%%'>%s</span>\n<span alpha='60%%' size='small'>%s</span>",
                                     safe_title, safe_subtitle);
        } else {
            markup = g_strdup_printf("<span alpha='75%%'>%s</span>", safe_title);
        }
        gtk_widget_set_visible(star_btn, FALSE);
    } else if (*safe_subtitle) {
        markup = g_strdup_printf("%s\n<span alpha='65%%' size='small'>%s</span>",
                                 safe_title, safe_subtitle);
        gtk_widget_set_visible(star_btn, payload->type == SIDEBAR_ROW_WORD);
    } else {
        markup = g_strdup(safe_title);
        gtk_widget_set_visible(star_btn, payload->type == SIDEBAR_ROW_WORD);
    }

    gtk_label_set_markup(GTK_LABEL(label), markup);

    g_signal_handlers_disconnect_by_func(star_btn, on_sidebar_favorite_clicked, NULL);
    g_object_set_data(G_OBJECT(star_btn), "bind-item", item); // useful for re-evaluating visibility if favorite state changes
    
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
            char hc = haystack[h_idx];
            
            /* Fast-skip common DSL formatting characters, punctuation and whitespace during the match */
            if (g_ascii_isspace(hc) || 
                hc == '{' || hc == '}' || hc == '\\' || hc == '~' || 
                hc == '/' || hc == ',' || hc == '.' || hc == '-' || 
                hc == '(' || hc == ')' || hc == '[' || hc == ']' || hc == '_') {
                h_idx++;
                continue;
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

static gboolean continue_sidebar_search(gpointer user_data) {
    SidebarSearchState *state = user_data;
    if (!state || state != sidebar_search_state) {
        return G_SOURCE_REMOVE;
    }

    guint processed = 0;
    const guint max_batch_size = 512;
    gint64 deadline_us = g_get_monotonic_time() + 2000;
    while (processed < max_batch_size && g_get_monotonic_time() < deadline_us) {
        if (!state->current_entry && !state->has_current_pos) {
            // END OF SEARCH - DO GLOBAL SORT & FLUSH
            for (int i = 0; i < BUCKET_COUNT; i++) {
                guint n = state->global_bucket_labels[i]->len;
                if (n > 1) {
                    BucketItem *items = g_new(BucketItem, n);
                    for (guint j = 0; j < n; j++) {
                        char *label = g_ptr_array_index(state->global_bucket_labels[i], j);
                        items[j].label = label;
                        items[j].sort_key = g_utf8_casefold(label, -1);
                        items[j].payload = g_ptr_array_index(state->global_bucket_payloads[i], j);
                        items[j].score = items[j].payload ? items[j].payload->fuzzy_score : 0.0;
                    }
                    g_sort_array(items, n, sizeof(BucketItem), compare_bucket_item, GINT_TO_POINTER(i));
                    for (guint j = 0; j < n; j++) {
                        g_ptr_array_index(state->global_bucket_labels[i], j) = items[j].label;
                        g_ptr_array_index(state->global_bucket_payloads[i], j) = items[j].payload;
                        g_free(items[j].sort_key);
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
            }

            if (!state->list_started &&
                g_hash_table_size(state->seen_words) == 0) {
                populate_search_sidebar_status("No results", NULL);
            }

            state->source_id = 0;
            g_clear_pointer(&sidebar_search_state, sidebar_search_state_free);
            return G_SOURCE_REMOVE;
        }

        if (!state->has_current_pos) {
            gboolean found_dict = FALSE;
            while (state->current_entry) {
                DictEntry *entry = state->current_entry;
                state->current_entry = state->current_entry->next;
                if (!entry->dict || !entry->dict->index ||
                    flat_index_count(entry->dict->index) == 0 ||
                    !dict_entry_in_active_scope(entry)) {
                    continue;
                }
                state->current_pos = 0;
                state->has_current_pos = TRUE;
                state->current_dict = entry;
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

        const FlatTreeEntry *node = flat_index_get(state->current_dict->dict->index, state->current_pos);
        if (!node) {
            state->has_current_pos = FALSE;
            continue;
        }
        state->current_pos++;
        if (state->current_pos >= flat_index_count(state->current_dict->dict->index)) {
            state->has_current_pos = FALSE;
        }

        // FAST PRE-FILTER (zero alloc, length-safe)
        if (!fast_strncasestr(state->current_dict->dict->data + node->h_off, node->h_len, state->query)) {
            processed++;
            if ((processed & 63) == 0) {
                if (g_get_monotonic_time() > deadline_us)
                    break;
            }
            continue;
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

            if (classify_search_candidate(state->query_key, state->query_len, word_key, &bucket, &fuzzy_score)) {
                if (!g_hash_table_contains(state->seen_words, word_key)) {
                    RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                    payload->type = RELATED_ROW_CANDIDATE;
                    
                    /* Use rendering normalization for display in sidebar */
                    char *display_word = normalize_headword_for_render(word, node->h_len, TRUE);
                    payload->word = display_word; 
                    
                    payload->fuzzy_score = fuzzy_score;
                    g_hash_table_add(state->seen_words, g_strdup(word_key));
                
                int b = (int)bucket;
                if (b >= 0 && b < BUCKET_COUNT) {
                    g_ptr_array_add(state->global_bucket_labels[b], display_word); 
                    g_ptr_array_add(state->global_bucket_payloads[b], payload);
                    clean_word = NULL;
                    display_word = NULL;

                    // 🔥 PROGRESSIVE FLUSH (per bucket)
                    if ((state->global_bucket_labels[b]->len & 31) == 0) {
                        guint n = state->global_bucket_labels[b]->len;
                        if (n > 1) {
                            BucketItem *items = g_new(BucketItem, n);
                            for (guint j = 0; j < n; j++) {
                                char *label = g_ptr_array_index(state->global_bucket_labels[b], j);
                                items[j].label = label;
                                items[j].sort_key = g_utf8_casefold(label, -1);
                                items[j].payload = g_ptr_array_index(state->global_bucket_payloads[b], j);
                                items[j].score = items[j].payload ? items[j].payload->fuzzy_score : 0.0;
                            }

                            g_sort_array(items, n, sizeof(BucketItem), compare_bucket_item, GINT_TO_POINTER(b));

                            for (guint j = 0; j < n; j++) {
                                g_ptr_array_index(state->global_bucket_labels[b], j) = items[j].label;
                                g_ptr_array_index(state->global_bucket_payloads[b], j) = items[j].payload;
                                g_free(items[j].sort_key);
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
            state->current_dict = NULL;
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
    const guint max_seed_rows = 10000;
    guint added = 0;

    for (DictEntry *entry = all_dicts; entry && added < max_seed_rows; entry = entry->next) {
        if (!entry->dict || !entry->dict->index || !dict_entry_in_active_scope(entry)) {
            continue;
        }

        size_t pos = flat_index_search_prefix(entry->dict->index, state->query);
        while (pos != (size_t)-1 && added < max_seed_rows) {
            const FlatTreeEntry *node = flat_index_get(entry->dict->index, pos);
            if (!node) break;

            char *raw_word = g_strndup(entry->dict->data + node->h_off, node->h_len);
            char *clean_word = normalize_headword_for_search(raw_word, TRUE);
            g_free(raw_word);

            if (!clean_word || text_has_replacement_char(clean_word)) {
                if (clean_word) g_free(clean_word);
                pos++;
                if (pos >= flat_index_count(entry->dict->index)) break;
                continue;
            }

            char *word_key = g_utf8_casefold(clean_word, -1);
            SearchBucket bucket;
            double score;

            if (classify_search_candidate(state->query_key, state->query_len, word_key, &bucket, &score)) {
                if (bucket == SEARCH_BUCKET_EXACT || bucket == SEARCH_BUCKET_PREFIX) {
                    if (!g_hash_table_contains(state->seen_words, word_key)) {
                        RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                        payload->type = RELATED_ROW_CANDIDATE;
                        payload->word = clean_word;
                        payload->fuzzy_score = score;

                        g_hash_table_add(state->seen_words, g_strdup(word_key));
                        g_ptr_array_add(labels, clean_word);
                        g_ptr_array_add(payloads, payload);
                        added++;
                        clean_word = NULL;
                    }
                } else {
                    g_free(word_key);
                    if (clean_word) g_free(clean_word);
                    break;
                }
            }

            if (clean_word) g_free(clean_word);
            g_free(word_key);
            pos++;
            if (pos >= flat_index_count(entry->dict->index)) break;
        }
    }

    if (labels->len > 0) {
        set_related_rows(labels, payloads);
        state->list_started = TRUE;
    }

    g_ptr_array_free(labels, TRUE);
    g_ptr_array_free(payloads, TRUE);

    return added;
}

static void populate_search_sidebar(const char *query) {
    cancel_sidebar_search();

    char *clean = normalize_headword_for_search(query, FALSE);
    // If clean is NULL, it means the query is empty or whitespace-only.
    // We allow this to show all headwords.

    sidebar_search_state = g_new0(SidebarSearchState, 1);
    sidebar_search_state->query = clean ? clean : g_strdup("");
    sidebar_search_state->query_key = g_utf8_casefold(sidebar_search_state->query, -1);
    sidebar_search_state->query_len = utf8_length_or_bytes(sidebar_search_state->query_key);
    sidebar_search_state->seen_words = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL);
    sidebar_search_state->current_entry = all_dicts;

    for (int i = 0; i < BUCKET_COUNT; i++) {
        sidebar_search_state->global_bucket_labels[i] = g_ptr_array_new();
        sidebar_search_state->global_bucket_payloads[i] = g_ptr_array_new();
    }

    if (seed_search_sidebar_fast_rows(sidebar_search_state) == 0) {
        populate_search_sidebar_status("Searching…", NULL);
    }
    sidebar_search_state->source_id = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE, continue_sidebar_search, sidebar_search_state, NULL);
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
    DictConfig *cfg = settings_find_dictionary_by_path(app_settings, entry->path);
    return !cfg || cfg->enabled;
}

static gboolean dict_entry_in_scope(DictEntry *entry, const char *scope_id) {
    if (!entry || !dict_entry_enabled(entry)) {
        return FALSE;
    }
    if (!scope_id || g_strcmp0(scope_id, "all") == 0 || !app_settings) {
        return TRUE;
    }

    char *dict_id = settings_make_dictionary_id(entry->path);
    gboolean allowed = FALSE;
    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        if (g_strcmp0(grp->id, scope_id) != 0) {
            continue;
        }
        for (guint j = 0; j < grp->members->len; j++) {
            const char *member = g_ptr_array_index(grp->members, j);
            if (g_strcmp0(member, dict_id) == 0) {
                allowed = TRUE;
                break;
            }
        }
        break;
    }
    g_free(dict_id);
    return allowed;
}

static gboolean dict_entry_in_active_scope(DictEntry *entry) {
    return dict_entry_in_scope(entry, active_scope_id);
}

static gboolean path_is_inside_directory(const char *path, const char *dir) {
    if (!path || !dir || !*path || !*dir) {
        return FALSE;
    }

    gsize dir_len = strlen(dir);
    if (!g_str_has_prefix(path, dir)) {
        return FALSE;
    }

    if (path[dir_len] == '\0') {
        return TRUE;
    }

    return dir[dir_len - 1] == G_DIR_SEPARATOR || path[dir_len] == G_DIR_SEPARATOR;
}

static DictEntry *dict_entry_new_shell(const char *name, const char *path) {
    if (!path || !*path) {
        return NULL;
    }

    DictEntry *entry = calloc(1, sizeof(DictEntry));
    entry->format = dict_detect_format(path);
    entry->path = strdup(path);
    entry->name = strdup((name && *name) ? name : path);
    return entry;
}

static DictEntry *find_dict_entry_by_path(const char *path) {
    if (!path) {
        return NULL;
    }

    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        if (g_strcmp0(entry->path, path) == 0) {
            return entry;
        }
    }

    return NULL;
}

static guint rebuild_dict_entries_from_settings(void) {
    DictEntry *old_head = all_dicts;
    DictEntry *new_head = NULL;
    DictEntry *new_tail = NULL;
    guint count = 0;
    char *active_path = active_entry && active_entry->path ? g_strdup(active_entry->path) : NULL;

    GHashTable *existing_by_path = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *reused_entries = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (DictEntry *entry = old_head; entry; entry = entry->next) {
        if (entry->path && !g_hash_table_contains(existing_by_path, entry->path)) {
            g_hash_table_insert(existing_by_path, entry->path, entry);
        }
    }

    if (app_settings) {
        for (guint i = 0; i < app_settings->dictionaries->len; i++) {
            DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, i);
            if (!cfg || !cfg->path || !*cfg->path) {
                continue;
            }

            DictEntry *entry = g_hash_table_lookup(existing_by_path, cfg->path);
            if (!entry) {
                entry = dict_entry_new_shell(cfg->name, cfg->path);
            } else {
                g_hash_table_add(reused_entries, entry);
                free(entry->name);
                entry->name = strdup((cfg->name && *cfg->name) ? cfg->name : cfg->path);
                if (g_strcmp0(entry->path, cfg->path) != 0) {
                    free(entry->path);
                    entry->path = strdup(cfg->path);
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

    for (DictEntry *entry = old_head; entry; ) {
        DictEntry *next = entry->next;
        if (!g_hash_table_contains(reused_entries, entry)) {
            entry->next = NULL;
            dict_loader_free(entry);
        }
        entry = next;
    }

    g_hash_table_unref(existing_by_path);
    g_hash_table_unref(reused_entries);

    all_dicts = new_head;
    active_entry = active_path ? find_dict_entry_by_path(active_path) : NULL;
    if (!active_entry) {
        active_entry = all_dicts;
    }
    g_free(active_path);
    return count;
}

static gboolean should_rescan_dictionary_dirs(void) {
    if (!app_settings || app_settings->dictionary_dirs->len == 0) {
        return FALSE;
    }

    for (guint i = 0; i < app_settings->dictionary_dirs->len; i++) {
        const char *dir = g_ptr_array_index(app_settings->dictionary_dirs, i);
        gboolean indexed = FALSE;

        for (guint j = 0; j < app_settings->dictionaries->len; j++) {
            DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, j);
            if (!cfg || !cfg->path || !*cfg->path) {
                continue;
            }
            if (g_strcmp0(cfg->source, "directory") == 0 &&
                path_is_inside_directory(cfg->path, dir)) {
                indexed = TRUE;
                break;
            }
        }

        if (!indexed) {
            for (guint j = 0; j < app_settings->ignored_dictionary_paths->len; j++) {
                const char *ignored = g_ptr_array_index(app_settings->ignored_dictionary_paths, j);
                if (path_is_inside_directory(ignored, dir)) {
                    indexed = TRUE;
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
    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        if (!entry->dict || !entry->path) {
            continue;
        }
        settings_upsert_dictionary(app_settings, entry->name, entry->path, "directory");
        g_hash_table_add(loaded_paths, g_strdup(entry->path));
    }

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
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *payloads = g_ptr_array_new();
    SidebarRowPayload *active_payload = NULL;

    guint all_count = 0;
    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        if (dict_entry_enabled(entry)) {
            all_count++;
        }
    }

    SidebarRowPayload *all_payload = g_new0(SidebarRowPayload, 1);
    char all_subtitle[64];
    g_snprintf(all_subtitle, sizeof(all_subtitle), "%u dictionaries", all_count);
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
        return;
    }

    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        guint member_count = 0;
        for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
            if (!dict_entry_enabled(entry)) {
                continue;
            }
            char *dict_id = settings_make_dictionary_id(entry->path);
            for (guint j = 0; j < grp->members->len; j++) {
                const char *member = g_ptr_array_index(grp->members, j);
                if (g_strcmp0(member, dict_id) == 0) {
                    member_count++;
                    break;
                }
            }
            g_free(dict_id);
        }

        SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
        char subtitle[64];
        g_snprintf(subtitle, sizeof(subtitle), "%u dictionaries", member_count);
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

static void on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType t, gpointer user_data) {
    (void)v;
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
        } else if (g_strcmp0(uri, "file:///") != 0) {
            fprintf(stderr, "[LINK CLICKED]: '%s'\n", uri);
        }
        if (g_str_has_prefix(uri, "dict://")) {
            const char *word = uri + 7;
            char *unescaped = g_uri_unescape_string(word, NULL);
            char *clean_link = normalize_headword_for_search(unescaped ? unescaped : word, TRUE);
            gtk_editable_set_text(GTK_EDITABLE(user_data), clean_link ? clean_link : (unescaped ? unescaped : word));
            g_free(clean_link);
            g_free(unescaped);
            webkit_policy_decision_ignore(d);
            return;
        } else if (g_str_has_prefix(uri, "sound://")) {
            if (!try_play_encoded_sound_uri(uri)) {
                const char *sound_file = uri + 8; // Skip "sound://"
                fprintf(stderr, "[AUDIO CLICKED] Clicked: %s\n", sound_file);
                
                /* Backward-compatible fallback and lazy loading */
                if (active_entry && active_entry->dict && active_entry->dict->resource_dir) {
                    char *audio_path = NULL;
                    if (active_entry->dict->resource_reader) {
                        audio_path = resource_reader_get(active_entry->dict->resource_reader, sound_file);
                    }
                    if (!audio_path) {
                        audio_path = g_build_filename(active_entry->dict->resource_dir, sound_file, NULL);
                    }

                    if (audio_path && g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
                        play_audio_file(audio_path);
                    } else {
                        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path ? audio_path : sound_file);
                    }
                    g_free(audio_path);
                } else {
                    fprintf(stderr, "[AUDIO ERROR] No active dictionary or resource directory\n");
                }
            }
            
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
    gtk_widget_add_css_class(GTK_WIDGET(sidebar->list_view), "navigation-sidebar");
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
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_use_markup(GTK_LABEL(label), TRUE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_top(label, 8);
    gtk_widget_set_margin_bottom(label, 8);
    
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
    g_free(active_scope_id);
    active_scope_id = g_strdup(payload->scope_id ? payload->scope_id : "all");
    refresh_search_results();
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

static gboolean headword_matches_normalized_query(const char *raw_word, const char *query_key) {
    gboolean matches = FALSE;
    char *normalized = normalize_headword_for_search(raw_word, TRUE);
    if (normalized) {
        char *normalized_key = g_utf8_casefold(normalized, -1);
        matches = g_strcmp0(normalized_key, query_key) == 0;
        g_free(normalized_key);
        g_free(normalized);
    }
    return matches;
}

static void append_rendered_entry_html(GString *html_res,
                                       DictEntry *entry,
                                       const FlatTreeEntry *res,
                                       int dict_idx,
                                       int *dict_header_shown,
                                       int *found_count) {
    const char *def_ptr = entry->dict->data + res->d_off;
    size_t def_len = res->d_len;

    if (entry->format == DICT_FORMAT_MDX && def_len > 8 && g_str_has_prefix(def_ptr, "@@@LINK=")) {
        char link_target[1024];
        const char *lp = def_ptr + 8;
        size_t l = 0;
        while (l < sizeof(link_target) - 1 && l < (def_len - 8) && lp[l] != '\r' && lp[l] != '\n') {
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
        }
    }

    int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
    const char *render_style = (app_settings && app_settings->render_style && *app_settings->render_style)
        ? app_settings->render_style
        : "diction";

    /* Phase 2: Set lazy resource reader for on-demand extraction */
    dict_render_set_resource_reader(entry->dict->resource_reader);

    char *rendered = dsl_render_to_html(
        def_ptr, def_len,
        entry->dict->data + res->h_off, res->h_len,
        entry->format, entry->dict->resource_dir, entry->dict->source_dir, entry->dict->mdx_stylesheet, dark_mode,
        app_settings ? app_settings->color_theme : "default",
        render_style,
        app_settings ? app_settings->font_family : NULL,
        app_settings ? app_settings->font_size : 0);
    if (!rendered) {
        return;
    }

    char *escaped_name = safe_markup_escape_n(entry->name ? entry->name : "", -1);
    char *tmp_hw = g_strndup(entry->dict->data + res->h_off, res->h_len);
    char *clean_hw = normalize_headword_for_search(tmp_hw, TRUE);
    char *escaped_headword = safe_markup_escape_n(clean_hw ? clean_hw : tmp_hw, -1);
    g_free(tmp_hw);
    if (clean_hw) g_free(clean_hw);
    gboolean first_match_for_dict = FALSE;

    if (!*dict_header_shown) {
        *dict_header_shown = 1;
        entry->has_matches = TRUE;
        (*found_count)++;
        first_match_for_dict = TRUE;
    }

    if (first_match_for_dict) {
        g_string_append_printf(
            html_res,
            "<div id='dict-%d' class='dict-anchor' style='scroll-margin-top: 8px;'></div>",
            dict_idx);
    }

    if (g_strcmp0(render_style, "python") == 0) {
        g_string_append_printf(
            html_res,
            "<div class='entry'><div class='header'><div><span class='lemma'>%s</span></div>"
            "<span class='dict'>📖 %s</span></div><div class='defs'>%s</div><hr></div>",
            escaped_headword, escaped_name, rendered);
    } else if (g_strcmp0(render_style, "goldendict-ng") == 0) {
        g_string_append_printf(
            html_res,
            "<article class='gdarticle'><div class='gold-header'><span class='gold-entry-headword'>%s</span>"
            "<span class='gold-dict'>📖 %s</span></div><div class='gdarticlebody'>%s</div></article>",
            escaped_headword, escaped_name, rendered);
    } else if (g_strcmp0(render_style, "slate-card") == 0) {
        g_string_append_printf(
            html_res,
            "<section class='slate-entry'><div class='slate-header'><span class='slate-lemma'>%s</span>"
            "<span class='slate-dict'>📖 %s</span></div><div class='slate-entry-body'>%s</div></section>",
            escaped_headword, escaped_name, rendered);
    } else if (g_strcmp0(render_style, "paper") == 0) {
        g_string_append_printf(
            html_res,
            "<section class='paper-entry'><div class='paper-header'><span class='paper-lemma'>%s</span>"
            "<span class='paper-dict'>📖 %s</span></div><div class='paper-entry-body'>%s</div></section>",
            escaped_headword, escaped_name, rendered);
    } else {
        g_string_append_printf(
            html_res,
            "<section class='diction-entry'><div class='diction-header'><span class='diction-lemma'>%s</span>"
            "<span class='diction-dict'>📖 %s</span></div><div class='diction-entry-body'>%s</div></section>",
            escaped_headword, escaped_name, rendered);
    }

    g_free(escaped_headword);
    g_free(escaped_name);
    free(rendered);
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

static void push_to_nav_history(const char *view_word, const char *search_query) {
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
            g_ascii_strcasecmp(current->search_query, clean_query) == 0) {
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
            g_ascii_strcasecmp(last->search_query, clean_query) == 0) {
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
    g_ptr_array_add(hist, item);
    set_current_nav_index(hist->len - 1);
    update_nav_buttons_state();
}

static void execute_search_now_for_query(const char *query_raw, gboolean push_history);

static void navigate_to_history_item(NavHistoryItem *item) {
    if (g_ascii_strcasecmp(item->view_word, item->search_query) == 0) {
        populate_search_sidebar(item->search_query);
        execute_search_now_for_query(item->search_query, FALSE);
    } else {
        execute_search_now_for_query(item->view_word, FALSE);
        populate_search_sidebar(item->search_query);
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

static void execute_search_now_for_query(const char *query_raw, gboolean push_history) {
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }

    if (!search_entry) {
        return;
    }

    char *query = normalize_headword_for_search(query_raw, FALSE);

    if (query && *query) {
        fprintf(stderr, "[Query]: '%s'\n", query);
    }



    if (!query || strlen(query) == 0) {
        cancel_sidebar_search();
        populate_search_sidebar(NULL);
        WebKitWebView *wv = get_current_web_view();
        if (wv) webkit_web_view_load_html(wv, "<h2>Diction</h2><p>Start typing to search...</p>", "file:///");
        for (DictEntry *e = all_dicts; e; e = e->next) {
            e->has_matches = FALSE;
        }
        populate_dict_sidebar();
        g_free(query);
        return;
    }



    GString *html_res = g_string_new("<html><body>");
    char *escaped_query_attr = safe_markup_escape_n(query, -1);
    g_string_append_printf(html_res, "<div class='word-group' data-word='%s'>", escaped_query_attr);
    g_free(escaped_query_attr);

    int found_count = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) e->has_matches = FALSE;

    char *query_key = g_utf8_casefold(query, -1);
    int dict_idx = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (!e->dict || !dict_entry_in_active_scope(e)) continue;

        size_t pos = flat_index_search(e->dict->index, query);
        int dict_header_shown = 0;

        while (pos != (size_t)-1) {
            const FlatTreeEntry *res = flat_index_get(e->dict->index, pos);
            if (!res) break;

            /* Verify we are still within the agnostic-match group */
            if (compare_headword(e->dict->data, res, query, strlen(query)) != 0) break;

            char *raw_word = g_strndup(e->dict->data + res->h_off, res->h_len);
            gboolean matches = headword_matches_normalized_query(raw_word, query_key);
            g_free(raw_word);

            if (matches) {
                append_rendered_entry_html(html_res, e, res, dict_idx, &dict_header_shown, &found_count);
            }
            
            pos++;
            if (pos >= flat_index_count(e->dict->index)) break;
        }

        dict_idx++;
    }

    if (found_count > 0) {
        g_string_append(html_res, "</div></body></html>");
        WebKitWebView *wv = get_current_web_view();
        if (wv) webkit_web_view_load_html(wv, html_res->str, "file:///");
        if (push_history) {
            update_history_word(query);
            const char *current_search_query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
            push_to_nav_history(query, current_search_query);
        }
    } else {
        guint enabled_dicts = 0;
        guint loaded_dicts = 0;
        for (DictEntry *e = all_dicts; e; e = e->next) {
            if (!dict_entry_in_active_scope(e)) {
                continue;
            }
            enabled_dicts++;
            if (e->dict && e->dict->index) {
                loaded_dicts++;
            }
        }

        // Theme-aware no results message
        int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
        const char *text_color = dark_mode ? "#aaaaaa" : "#666666";

        char *escaped_query = safe_markup_escape_n(query, -1);
        char buf[768];
        snprintf(buf, sizeof(buf),
            "<div style='padding: 20px; color: %s; font-style: italic;'>"
            "No exact match for <b>%s</b> in any dictionary.</div>", text_color, query);
        if (escaped_query) {
            snprintf(buf, sizeof(buf),
                "<div style='padding: 20px; color: %s; font-style: italic;'>"
                "No exact match for <b>%s</b> in any dictionary.</div>", text_color, escaped_query);
        }
        if (dictionary_loading_in_progress && loaded_dicts < enabled_dicts) {
            char loading_note[256];
            g_snprintf(loading_note, sizeof(loading_note),
                "<div style='padding: 0 20px 20px 20px; color: %s; opacity: 0.72;'>"
                "Still loading %u of %u dictionaries in the background.</div>",
                text_color, loaded_dicts, enabled_dicts);
            g_strlcat(buf, loading_note, sizeof(buf));
        }
        WebKitWebView *wv = get_current_web_view();
        if (wv) webkit_web_view_load_html(wv, buf, "file:///");
        g_free(escaped_query);
    }
    g_string_free(html_res, TRUE);

    populate_dict_sidebar();
    populate_search_sidebar(query);
    g_free(query_key);
    g_free(query);
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

static void append_rendered_word_html_impl(const char *raw_word, gboolean push_history) {
    char *query = normalize_headword_for_search(raw_word, TRUE);
    if (!query || strlen(query) == 0) {
        g_free(query);
        return;
    }

    char *display_title = normalize_headword_for_render(raw_word, raw_word ? strlen(raw_word) : 0, TRUE);

    GString *html_res = g_string_new("");
    int found_count = 0;
    char *query_key = g_utf8_casefold(query, -1);
    int dict_idx = 0;
    
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (!e->dict || !dict_entry_in_active_scope(e)) continue;

        size_t pos = flat_index_search(e->dict->index, query);
        int dict_header_shown = 0;

        while (pos != (size_t)-1) {
            const FlatTreeEntry *res = flat_index_get(e->dict->index, pos);
            if (!res) break;

            /* Verify we are still within the agnostic-match group */
            if (compare_headword(e->dict->data, res, query, strlen(query)) != 0) break;

            char *tmp_hw = g_strndup(e->dict->data + res->h_off, res->h_len);
            char *clean_hw = normalize_headword_for_search(tmp_hw, TRUE);
            gboolean matches = (clean_hw && g_ascii_strcasecmp(clean_hw, query) == 0);
            g_free(tmp_hw);
            if (clean_hw) g_free(clean_hw);

            if (matches) {
                append_rendered_entry_html(html_res, e, res, dict_idx, &dict_header_shown, &found_count);
            }
            pos++;
            if (pos >= flat_index_count(e->dict->index)) break;
        }
        dict_idx++;
    }

    if (found_count > 0) {
        if (tab_view) {
            AdwTabPage *page = adw_tab_view_get_selected_page(tab_view);
            if (page) {
                adw_tab_page_set_title(page, display_title);
                g_object_set_data_full(G_OBJECT(page), "search-query", g_strdup(query), g_free);
                g_object_set_data(G_OBJECT(page), "is-firm", GINT_TO_POINTER(1));
            }
        }
        
        if (push_history) {
            update_history_word(query);
            const char *current_search_query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
            push_to_nav_history(query, current_search_query);
        }

        char *b64_html = g_base64_encode((const guchar *)html_res->str, html_res->len);
        char *b64_word = g_base64_encode((const guchar *)query, strlen(query));

        WebKitWebView *wv = get_current_web_view();
        if (wv) webkit_web_view_load_html(wv, html_res->str, "file:///");

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
    g_free(query_key);
    g_free(query);
    g_string_free(html_res, TRUE);
}

static void append_rendered_word_html(const char *raw_word) {
    append_rendered_word_html_impl(raw_word, TRUE);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
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

    if (last_search_query && strcmp(query, last_search_query) == 0) return;

    g_free(last_search_query);
    last_search_query = g_strdup(query);

    if (!query || strlen(query) == 0) {
        execute_search_now();
        return;
    }

    schedule_execute_search();
}

static void on_random_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (!all_dicts) return;

    // Count dicts in active scope
    int count = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (e->dict && e->dict->index && flat_index_count(e->dict->index) > 0 && dict_entry_in_active_scope(e)) {
            count++;
        }
    }
    if (count == 0) return;

    // Pick random dict
    int target = rand() % count;
    DictEntry *e = all_dicts;
    int cur = 0;
    while (e) {
        if (e->dict && e->dict->index && flat_index_count(e->dict->index) > 0 && dict_entry_in_active_scope(e)) {
            if (cur == target) break;
            cur++;
        }
        e = e->next;
    }

    if (e && e->dict && flat_index_count(e->dict->index) > 0) {
        int attempts = 0;
        const FlatTreeEntry *node = NULL;
        char *clean_hw = NULL;
        char *tmp_hw = NULL;

        while (attempts < 10) {
            node = flat_index_random(e->dict->index);
            if (!node) break;
            const char *word = e->dict->data + node->h_off;
            size_t len = node->h_len;
            tmp_hw = g_strndup(word, len);
            clean_hw = normalize_headword_for_search(tmp_hw, TRUE);
            
            if (clean_hw && *clean_hw && !text_has_replacement_char(clean_hw)) {
                break;
            }
            
            g_free(tmp_hw);
            g_free(clean_hw);
            tmp_hw = NULL;
            clean_hw = NULL;
            attempts++;
        }

        if (node && tmp_hw) {
            fprintf(stderr, "[Random word]: '%s'\n", clean_hw ? clean_hw : tmp_hw);
            
            g_signal_handlers_block_by_func(search_entry, on_search_changed, NULL);
            char *rendered_hw = normalize_headword_for_render(tmp_hw, tmp_hw ? strlen(tmp_hw) : 0, TRUE);
            gtk_editable_set_text(GTK_EDITABLE(search_entry), rendered_hw);
            if (search_button_label) {
                gtk_label_set_text(GTK_LABEL(search_button_label), rendered_hw);
            }
            g_free(rendered_hw);
            g_signal_handlers_unblock_by_func(search_entry, on_search_changed, NULL);

            g_free(tmp_hw);
            if (clean_hw) g_free(clean_hw);
            // Search will be triggered by "search-changed" signal, but we want it instantly
            execute_search_now();
        }
    }
}

static void maybe_show_startup_random_word(void) {
    if (!startup_random_word_pending || !search_entry) {
        return;
    }


    const char *current = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (current && *current) {
        startup_random_word_pending = FALSE;
        return;
    }

    int loaded_count = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (e->dict && e->dict->index && flat_index_count(e->dict->index) > 0 && dict_entry_in_active_scope(e)) {
            loaded_count++;
            break;
        }
    }

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
    for (DictEntry *cursor = all_dicts; cursor; cursor = cursor->next) {
        if (!cursor->dict || !dict_entry_in_active_scope(cursor)) {
            continue;
        }
        if (cursor == e) {
            idx = current;
            break;
        }
        current++;
    }
    if (idx < 0) {
        return;
    }

    char js[256];
    snprintf(js, sizeof(js),
        "var el = document.getElementById('dict-%d'); "
        "if (el) { el.scrollIntoView({behavior: 'smooth', block: 'start'}); }",
        idx);
    webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);
    active_entry = e;
}

// Refresh the current search results when theme changes
static void refresh_search_results(void) {
    if (!search_entry) return;

    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (!query || strlen(query) == 0) {
        // Refresh placeholder
        int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
        const char *bg = dark_mode ? "#1e1e1e" : "#ffffff";
        const char *fg = dark_mode ? "#dddddd" : "#222222";
        char html[256];
        snprintf(html, sizeof(html),
            "<html><body style='font-family: sans-serif; background: %s; color: %s; "
            "text-align: center; margin-top: 2em; opacity: 0.7;'>"
            "<h2>Diction</h2><p>Start typing to search...</p></body></html>",
            bg, fg);
        webkit_web_view_load_html(web_view, html, "file:///");
        return;
    }

    execute_search_now();
}

static double shift_color_component(double val, double amount, int darken) {
    if (darken) return CLAMP(val - amount, 0.0, 1.0);
    return CLAMP(val + amount, 0.0, 1.0);
}

static void update_theme_colors(void) {
    if (!web_view || !app_settings) return;

    int dark_mode = adw_style_manager_get_dark(adw_style_manager_get_default()) ? 1 : 0;

    dsl_theme_palette palette;
    dict_render_get_theme_palette(app_settings->color_theme, dark_mode, &palette);

    /* Update WebKit background to match palette */
    GdkRGBA bg_color;
    if (!gdk_rgba_parse(&bg_color, palette.bg))
        gdk_rgba_parse(&bg_color, dark_mode ? "#1e1e1e" : "#ffffff");
    webkit_web_view_set_background_color(web_view, &bg_color);

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
    char hover_color[32];
    g_snprintf(hover_color, sizeof(hover_color),
               "rgba(%u,%u,%u,0.15)", ar, ag, ab);
    char select_color[32];
    g_snprintf(select_color, sizeof(select_color),
               "rgba(%u,%u,%u,0.25)", ar, ag, ab);

    /*
     * Use Adwaita's @define-color mechanism so the theme engine picks up
     * our palette for its own rules (borders, shadows, transitions, etc.).
     * Direct property overrides are added below as a belt-and-suspenders
     * fallback, but @define-color is what actually moves the needle.
     */
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
        "window.background {\n"
        "  background-color: %s;\n"
        "  color: %s;\n"
        "}\n"
        "headerbar {\n"
        "  background-color: %s;\n" /* Content header color */
        "  color: %s;\n"
        "  border-bottom: none;\n"
        "}\n"
        ".sidebar, .navigation-sidebar, .sidebar listview, .navigation-sidebar listview, .sidebar list, .navigation-sidebar list, .sidebar scrolledwindow, .navigation-sidebar scrolledwindow {\n"
        "  background-color: %s;\n"
        "  border: none;\n"
        "}\n"
        ".navigation-sidebar {\n"
        "  border-right: 1px solid %s;\n"
        "}\n"
        ".sidebar {\n"
        "  border: none;\n"
        "}\n"
        "row, listitem {\n"
        "  color: %s;\n"
        "}\n"
        "row:selected, listitem:selected {\n"
        "  background-color: %s;\n"
        "  color: %s;\n"
        "}\n"
        "row:hover:not(:selected), listitem:hover:not(:selected) {\n"
        "  background-color: %s;\n"
        "}\n"
        "popover, popovermenu {\n"
        "  background-color: transparent;\n"
        "}\n"
        "popover > contents, popovermenu > contents {\n"
        "  background-color: %s;\n"
        "  color: %s;\n"
        "  padding: 0;\n"
        "}\n"
        "popover > contents row,\n"
        "popover > contents listitem {\n"
        "  color: %s;\n"
        "  background-color: transparent;\n"
        "}\n"
        "popover > contents row:hover:not(:selected),\n"
        "popover > contents listitem:hover:not(:selected) {\n"
        "  background-color: %s;\n"
        "}\n"
        "popover > contents row:checked {\n"
        "  color: %s;\n"
        "}\n",
        /* @define-color (10 args)*/
        palette.bg, palette.fg,
        palette.bg, palette.fg,
        palette.bg, palette.fg,
        c_chrome, palette.fg,
        c_surface, palette.fg,
        palette.accent,
        /* window/background (2) */
        palette.bg, palette.fg,
        /* default headerbar (2) */
        palette.bg, palette.fg,
        /* sidebar background (1) */
        c_chrome,
        /* sidebar border (1) */
        palette.border,
        /* row/listitem (1) */
        palette.fg,
        /* row:selected bg+fg (2) */
        select_color, palette.accent,
        /* row:hover (1) */
        hover_color,
        /* popover contents bg+fg (2) */
        c_surface, palette.fg,
        /* popover row color (1) */
        palette.fg,
        /* popover row hover (1) */
        hover_color,
        /* popover row:checked accent (1) */
        palette.accent
    );

    gtk_css_provider_load_from_string(dynamic_theme_provider, css);
    g_free(css);

    refresh_search_results();
}

static void on_style_manager_changed(AdwStyleManager *manager, GParamSpec *pspec, gpointer user_data) {
    (void)manager; (void)pspec; (void)user_data;
    update_theme_colors();
}

/* Called whenever font family or size changes in the Appearance tab */
static void apply_font_to_webview(void *user_data) {
    (void)user_data;
    if (!web_view || !app_settings) return;

    /* Also keep WebKit's default-font settings for non-styled pages */
    WebKitSettings *ws = webkit_web_view_get_settings(web_view);
    if (app_settings->font_family && *app_settings->font_family)
        webkit_settings_set_default_font_family(ws, app_settings->font_family);
    if (app_settings->font_size > 0)
        webkit_settings_set_default_font_size(ws, (guint32)app_settings->font_size);

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
                    "::selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::-webkit-selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::selection:inactive { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid #ff9f40 !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }",
                    ff, app_settings->font_size);
            else
                g_snprintf(css, sizeof(css),
                    "* { font-family: %s, sans-serif !important; }"
                    "body { font-size: %dpx !important; }"
                    "::selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::-webkit-selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::selection:inactive { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid #ff9f40 !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }",
                    ff, app_settings->font_size);
        } else {
            if (strchr(ff, ' ') && ff[0] != '\"' && ff[0] != '\'')
                g_snprintf(css, sizeof(css),
                    "* { font-family: \"%s\", sans-serif !important; }"
                    "::selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::-webkit-selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::selection:inactive { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid #ff9f40 !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }", ff);
            else
                g_snprintf(css, sizeof(css),
                    "* { font-family: %s, sans-serif !important; }"
                    "::selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::-webkit-selection { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "::selection:inactive { background-color: #ff9f40 !important; color: #000000 !important; }"
                    "mark { background-color: transparent !important; color: inherit !important; border-bottom: 2px solid #ff9f40 !important; }"
                    ".diction-match { background-color: #ffff00 !important; color: #000000 !important; }", ff);
        }

        font_user_stylesheet = webkit_user_style_sheet_new(
            css,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_STYLE_LEVEL_USER,
            NULL, NULL);
        webkit_user_content_manager_add_style_sheet(font_ucm, font_user_stylesheet);
    }

    /* Refresh rendered content so DSL pages also re-generate with new font */
    update_theme_colors();
}

static GMutex dict_loader_mutex;
static volatile gint loader_generation = 0;

static void reload_dictionaries_from_settings(void *user_data) {
    (void)user_data;
    gboolean discover_from_dirs = should_rescan_dictionary_dirs();
    startup_random_word_pending = FALSE;
    g_atomic_int_inc(&loader_generation);
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }
    cancel_sidebar_search();

    // Free existing dicts
    dict_loader_free(all_dicts);
    all_dicts = NULL;
    active_entry = NULL;

    // Clear sidebar
    clear_sidebar_list(&dict_sidebar);
    clear_related_rows();
    clear_sidebar_list(&groups_sidebar);
    dictionary_loading_in_progress = FALSE;

    // Show "Reloading..." and start async scan
    webkit_web_view_load_html(web_view,
        "<html><body style='font-family: sans-serif; text-align: center; margin-top: 3em; opacity: 0.6;'>"
        "<h2>Reloading dictionaries\u2026</h2><p>Please wait.</p>"
        "</body></html>", "file:///");

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
void request_loader_cancel(void) {
    g_atomic_int_inc(&loader_generation);
}

static void finalize_dictionary_loading(gboolean allow_random_word, gboolean sync_settings_from_loaded) {
    dictionary_loading_in_progress = FALSE;
    if (sync_settings_from_loaded) {
        sync_settings_dictionaries_from_loaded();
    }
    rebuild_dict_entries_from_settings();
    extern void settings_scan_notify(const char *name, const char *path, int event_type);
    settings_scan_notify(NULL, NULL, -1);
    if (!active_entry && all_dicts) {
        active_entry = all_dicts;
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
        const char *developers[] = { "Diction Contributors", NULL };
        AdwDialog *dialog = adw_about_dialog_new();
        adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), "Diction");
        adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(dialog), "A high-performance, multi-format offline dictionary.");
        adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), "0.1.0");
        adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(dialog), "Diction Contributors");
        adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(dialog), developers);
        adw_about_dialog_set_copyright(ADW_ABOUT_DIALOG(dialog), "© 2024 Diction Contributors");
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
    GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *payloads = g_ptr_array_new();
    SidebarRowPayload *active_payload = NULL;

    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (dict_entry_visible_in_sidebar(e)) {
            SidebarRowPayload *payload = g_new0(SidebarRowPayload, 1);
            payload->type = SIDEBAR_ROW_DICT;
            payload->title = g_strdup(e->name ? e->name : "Dictionary");
            payload->dict_entry = e;
            g_ptr_array_add(labels, g_strdup(payload->title));
            g_ptr_array_add(payloads, payload);
            if (e == active_entry) {
                active_payload = payload;
            }
        }
    }

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

static void collect_dictionary_candidate_paths_recursive(const char *dirpath,
                                                         GPtrArray *out_paths,
                                                         GHashTable *seen_paths,
                                                         GHashTable *seen_dsl_families,
                                                         GHashTable *ignored_paths) {
    if (!dirpath || !out_paths) {
        return;
    }

    GDir *dir = g_dir_open(dirpath, 0, NULL);
    if (!dir) {
        return;
    }

    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.') {
            continue;
        }

        char *full = g_build_filename(dirpath, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            if (!loader_path_ends_with_ci(full, ".files") &&
                !loader_path_ends_with_ci(full, ".dsl.files") &&
                !loader_path_ends_with_ci(full, ".dsl.dz.files")) {
                collect_dictionary_candidate_paths_recursive(full, out_paths,
                                                            seen_paths, seen_dsl_families,
                                                            ignored_paths);
            }
            g_free(full);
            continue;
        }

        loader_add_candidate_path(full, out_paths, seen_paths, seen_dsl_families, ignored_paths);
        g_free(full);
    }

    g_dir_close(dir);
}

static DictEntry *create_dict_entry_from_loaded(const char *path, DictFormat fmt, DictMmap *dict) {
    if (!path || !dict) {
        return NULL;
    }

    DictEntry *entry = calloc(1, sizeof(DictEntry));
    entry->format = fmt;
    entry->dict = dict;

    if (dict->name && *dict->name) {
        char *valid = g_utf8_make_valid(dict->name, -1);
        entry->name = strdup(valid);
        g_free(valid);
    } else {
        char *base = g_path_get_basename(path);
        char *valid = g_utf8_make_valid(base, -1);
        entry->name = strdup(valid);
        g_free(valid);
        g_free(base);
    }

    char *valid_path = g_utf8_make_valid(path, -1);
    entry->path = strdup(valid_path);
    g_free(valid_path);
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

    if (g_strcmp0(hw_lang, def_lang) == 0) {
        return g_strdup_printf("%s", hw_lang);
    } else {
        return g_strdup_printf("%s->%s", hw_lang, def_lang);
    }
}

static gboolean on_dict_loaded_idle(gpointer user_data) {
    LoadIdleData *ld = user_data;

    if (ld->generation != g_atomic_int_get(&loader_generation)) {
        if (ld->entry) {
            dict_loader_free(ld->entry);
        }
        g_free(ld->status_text);
        g_free(ld);
        return G_SOURCE_REMOVE;
    }

    update_startup_splash_progress(ld->completed, ld->total, ld->status_text);

    if (ld->kind == LOAD_IDLE_ENTRY && ld->entry) {
        DictEntry *e = ld->entry;
        e->next = NULL;

        /* Inform settings dialog(s) of finished entry */
        extern void settings_scan_notify(const char *name, const char *path, int event_type);
        settings_scan_notify(e->name ? e->name : "(Unknown)", e->path ? e->path : "", DICT_LOADER_EVENT_FINISHED);

        DictConfig *cfg = app_settings ? settings_find_dictionary_by_path(app_settings, e->path) : NULL;
        if (cfg && !cfg->enabled) {
            dict_loader_free(e);
            g_free(ld->status_text);
            g_free(ld);
            return G_SOURCE_REMOVE;
        }

        // Check for duplicate in global list (might exist if reload/re-scan happened)
        DictEntry *existing = NULL;
        for (DictEntry *curr = all_dicts; curr; curr = curr->next) {
            if (curr->path && strcmp(curr->path, e->path) == 0) {
                existing = curr;
                break;
            }
        }

        if (!existing && e->dict) {
            char *guessed = NULL;
            if (e->dict->source_lang && e->dict->target_lang) {
                guessed = g_strdup_printf("%s->%s", e->dict->source_lang, e->dict->target_lang);
            } else if (e->dict->source_lang) {
                guessed = g_strdup(e->dict->source_lang);
            } else {
                guessed = sample_dict_and_detect_lang(e);
            }

            if (guessed && *guessed) {
                e->guessed_lang_group = g_strdup(guessed);
                if (app_settings) {
                    char *dict_id = settings_make_dictionary_id(e->path);
                    if (settings_upsert_guessed_group(app_settings, guessed, dict_id)) {
                        populate_groups_sidebar();
                    }
                    g_free(dict_id);
                }
            }
            g_free(guessed);
            
            // Re-populate dict sidebar as well to show new group info if we decide to add it to subtitles
            populate_dict_sidebar();
        }

        if (existing) {
            // Already there, just update the loaded dict data
            // (The sidebar row already points to this 'existing' entry)
            if (existing->dict) dict_mmap_close(existing->dict);
            existing->dict = e->dict;
            if (e->name) {
                free(existing->name);
                existing->name = strdup(e->name);
            }
            // We can free the 'e' shell now as 'e->dict' is transferred
            e->dict = NULL;
            dict_loader_free(e);
        } else {
            // New unique entry
            if (!all_dicts) {
                all_dicts = e;
            } else {
                DictEntry *last = all_dicts;
                while (last->next) last = last->next;
                last->next = e;
            }
        }

        if (!active_entry && all_dicts) {
            active_entry = all_dicts;
        }

        if (!startup_loading_active) {
            populate_dict_sidebar();
        }

        maybe_show_startup_random_word();
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
    DictMmap *dict = dict_load_any(la->path, fmt, &loader_generation, la->generation);

    gint done = g_atomic_int_add(la->completed, 1) + 1;

    if (dict) {
        DictEntry *entry = create_dict_entry_from_loaded(la->path, fmt, dict);
        char *basename = g_path_get_basename(la->path);
        char *status = g_strdup_printf("Loading %s...", basename ? basename : "dictionary");
        queue_loader_idle(LOAD_IDLE_ENTRY, la->generation, (guint)done, la->total, status, entry,
                          la->discover_from_dirs);
        g_free(status);
        g_free(basename);
    }

    g_free(la);
}

static gpointer dict_load_thread(gpointer user_data) {
    LoadThreadArgs *args = user_data;
    GPtrArray *candidate_paths = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_mutex_lock(&dict_loader_mutex);
    if (args->generation != g_atomic_int_get(&loader_generation)) {
        g_mutex_unlock(&dict_loader_mutex);
        goto cleanup;
    }

    for (int i = 0; i < args->n_manual; i++) {
        const char *path = args->manual_paths[i];
        loader_add_candidate_path(path, candidate_paths, seen_paths, seen_dsl_families,
                                  args->ignored_paths);
    }

    for (int i = 0; i < args->n_dirs; i++) {
        if (args->generation != g_atomic_int_get(&loader_generation)) break;
        collect_dictionary_candidate_paths_recursive(args->dirs[i], candidate_paths,
                                                    seen_paths, seen_dsl_families,
                                                    args->ignored_paths);
    }

    /* Inform settings scan dialog of discovered candidates */
    extern void settings_scan_notify(const char *name, const char *path, int event_type);
    for (guint i = 0; i < candidate_paths->len; i++) {
        const char *p = g_ptr_array_index(candidate_paths, i);
        char *b = g_path_get_basename(p);
        settings_scan_notify(b, p, DICT_LOADER_EVENT_DISCOVERED);
        g_free(b);
    }

    guint total_candidates = candidate_paths->len;
    queue_loader_idle(LOAD_IDLE_STATUS, args->generation, 0, total_candidates,
                      total_candidates > 0 ? "Preparing Diction..." : "Preparing dictionary library...",
                      NULL, args->discover_from_dirs);

    /* ── Phase 5: Parallel dictionary loading ── */
    if (total_candidates > 0) {
        volatile gint completed_count = 0;

        /* Create thread pool with num_processors threads */
        guint n_workers = g_get_num_processors();
        if (n_workers < 2) n_workers = 2;
        if (n_workers > 8) n_workers = 8;

        GError *pool_error = NULL;
        GThreadPool *pool = g_thread_pool_new(load_one_dict_worker, NULL, (gint)n_workers, FALSE, &pool_error);

        if (pool) {
            for (guint i = 0; i < candidate_paths->len; i++) {
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
            for (guint i = 0; i < candidate_paths->len; i++) {
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
                }
            }
        }
    }

    g_mutex_unlock(&dict_loader_mutex);

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
    if (search_stack && search_entry) {
        gtk_stack_set_visible_child_name(search_stack, "entry");
        gtk_widget_grab_focus(GTK_WIDGET(search_entry));
    }
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
            gtk_stack_set_visible_child_name(search_stack, "entry");
            gtk_widget_grab_focus(GTK_WIDGET(search_entry));
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
    if (search_stack) {
        gtk_stack_set_visible_child_name(search_stack, "entry");
    }
    gtk_widget_grab_focus(GTK_WIDGET(search_entry));
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
    if (search_stack && search_entry) {
        gtk_stack_set_visible_child_name(search_stack, "entry");
        gtk_widget_grab_focus(GTK_WIDGET(search_entry));
    }
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
        GdkRGBA bg_color;
        gboolean is_dark = style_manager && adw_style_manager_get_dark(style_manager);
        if (is_dark) {
            gdk_rgba_parse(&bg_color, "#1e1e1e");
        } else {
            gdk_rgba_parse(&bg_color, "#ffffff");
        }
        if (app_settings->render_style &&
            (g_strcmp0(app_settings->render_style, "slate-card") == 0 ||
             g_strcmp0(app_settings->render_style, "paper") == 0 ||
             g_strcmp0(app_settings->render_style, "python") == 0 ||
             g_strcmp0(app_settings->render_style, "goldendict-ng") == 0)) {
            gdk_rgba_parse(&bg_color, is_dark ? "#1b1b1b" : "#f6f6f6");
        }
        webkit_web_view_set_background_color(wv, &bg_color);
    }

    g_signal_connect(wv, "decide-policy", G_CALLBACK(on_decide_policy), search_entry);
    WebKitFindController *fc = webkit_web_view_get_find_controller(wv);
    g_signal_connect(fc, "counted-matches", G_CALLBACK(on_find_counted_matches), NULL);

    GtkWidget *web_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(web_scroll, TRUE);
    gtk_widget_set_hexpand(web_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(web_scroll), GTK_WIDGET(wv));
    
    AdwTabPage *page = adw_tab_view_append(tab_view, web_scroll);
    adw_tab_page_set_title(page, title ? title : "Search");
    
    if (select_it) {
        adw_tab_view_set_selected_page(tab_view, page);
    }
    
    return page;
}

static void on_tab_selected(AdwTabView *view, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    AdwTabPage *page = adw_tab_view_get_selected_page(view);
    if (page && search_entry) {
        const char *query = (const char *)g_object_get_data(G_OBJECT(page), "search-query");
        
        g_signal_handlers_block_by_func(search_entry, on_search_changed, NULL);
        if (query) {
            char *display_query = normalize_headword_for_render(query, strlen(query), TRUE);
            gtk_editable_set_text(GTK_EDITABLE(search_entry), display_query);
            if (search_button_label) gtk_label_set_text(GTK_LABEL(search_button_label), display_query);
            g_free(display_query);
        } else {
            gtk_editable_set_text(GTK_EDITABLE(search_entry), "");
            if (search_button_label) gtk_label_set_text(GTK_LABEL(search_button_label), "Search");
        }
        g_signal_handlers_unblock_by_func(search_entry, on_search_changed, NULL);
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

    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(search_box, TRUE);

    nav_back_btn = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_add_css_class(nav_back_btn, "flat");
    g_signal_connect(nav_back_btn, "clicked", G_CALLBACK(on_nav_back_clicked), NULL);
    gtk_widget_set_sensitive(nav_back_btn, FALSE);

    nav_forward_btn = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(nav_forward_btn, "flat");
    g_signal_connect(nav_forward_btn, "clicked", G_CALLBACK(on_nav_forward_clicked), NULL);
    gtk_widget_set_sensitive(nav_forward_btn, FALSE);

    search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(search_entry), TRUE);
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);

    GtkEventController *focus_ctrl = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl, "leave", G_CALLBACK(on_search_entry_focus_leave), NULL);
    gtk_widget_add_controller(GTK_WIDGET(search_entry), focus_ctrl);

    GtkWidget *btn_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_content, GTK_ALIGN_FILL);
    GtkWidget *search_icon = gtk_image_new_from_icon_name("system-search-symbolic");
    gtk_widget_set_opacity(search_icon, 0.7); // make icon slightly dim
    search_button_label = GTK_LABEL(gtk_label_new("Search"));
    gtk_widget_set_opacity(GTK_WIDGET(search_button_label), 0.7); // make label text dim
    gtk_label_set_ellipsize(search_button_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(search_button_label, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(search_button_label), TRUE);
    // Move label to the left
    gtk_widget_set_halign(GTK_WIDGET(search_button_label), GTK_ALIGN_START);
    
    gtk_box_append(GTK_BOX(btn_content), search_icon);
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
    
    gtk_stack_add_named(search_stack, button_box, "button");
    gtk_stack_add_named(search_stack, GTK_WIDGET(search_entry), "entry");
    gtk_stack_set_visible_child_name(search_stack, "button");

    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), nav_back_btn);
    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), nav_forward_btn);

    gtk_box_append(GTK_BOX(search_box), GTK_WIDGET(search_stack));

    adw_header_bar_set_title_widget(ADW_HEADER_BAR(content_header), search_box);

    zoom_to_restore_btn = gtk_button_new_from_icon_name("window-maximize-symbolic");
    gtk_widget_add_css_class(zoom_to_restore_btn, "flat");
    gtk_widget_set_tooltip_text(zoom_to_restore_btn, "Restore normal size");
    g_signal_connect(zoom_to_restore_btn, "clicked", G_CALLBACK(on_zoom_to_restore_clicked), NULL);
    gtk_widget_set_visible(zoom_to_restore_btn, FALSE);
    adw_header_bar_pack_end(ADW_HEADER_BAR(content_header), zoom_to_restore_btn);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Preferences", "app.settings");
    GMenu *view_menu = g_menu_new();
    g_menu_append(view_menu, "Show/Hide Sidebar", "app.toggle-sidebar");
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

    g_signal_connect(tab_view, "notify::selected-page", G_CALLBACK(on_tab_selected), NULL);
    g_signal_connect(tab_view, "close-page", G_CALLBACK(on_tab_close), NULL);
    
    create_new_tab("Home", TRUE);

    GtkWidget *content_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(content_vbox), GTK_WIDGET(tab_bar));
    gtk_box_append(GTK_BOX(content_vbox), GTK_WIDGET(tab_view));
    gtk_box_append(GTK_BOX(content_vbox), create_find_bar());

    adw_toolbar_view_set_content(toolbar_view, content_vbox);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_view), GTK_WIDGET(toolbar_view));

    GSimpleAction *find_action = g_simple_action_new("find", NULL);
    g_signal_connect(find_action, "activate", G_CALLBACK(on_find_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(find_action));

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

    gboolean had_cached_entries = FALSE;
    gboolean discover_from_dirs = FALSE;
    if (!all_dicts && app_settings) {
        had_cached_entries = rebuild_dict_entries_from_settings() > 0;
        discover_from_dirs = should_rescan_dictionary_dirs();
    }

    /* Populate sidebar */
    populate_dict_sidebar();
    populate_history_sidebar();
    populate_favorites_sidebar();
    populate_groups_sidebar();
    populate_search_sidebar(NULL);


    /* Auto-select first dictionary */
    if (all_dicts) {
        active_entry = all_dicts;
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
        ".navigation-sidebar { background: transparent; }"
        ".navigation-sidebar listitem:hover, .navigation-sidebar row:hover { background: alpha(@theme_fg_color, 0.05); }"
        ".navigation-sidebar listitem:selected, .navigation-sidebar row:selected { background: alpha(@theme_fg_color, 0.1); color: inherit; }"
        ".content-header { background: @window_bg_color; }"
        ".menu-item { font-weight: normal; padding: 4px 8px; min-height: 0; }"
        "overlay-split-view > separator { background: @sidebar_bg_color; min-width: 1px; opacity: 1; }"
        "headerbar.sidebar { box-shadow: none; border-bottom: none; margin: 0; padding: 0; }"
        ".search-button-bg { background: alpha(@theme_fg_color, 0.08); border-radius: 6px; padding: 0px 8px; }"
        ".search-button-bg:hover { background: alpha(@theme_fg_color, 0.12); }"
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

    webkit_web_view_load_html(web_view,
        "<html><body style='font-family: sans-serif; text-align: center; margin-top: 3em; opacity: 0.6;'>"
        "<h2>Loading dictionaries…</h2><p>Please wait.</p>"
        "</body></html>", "file:///");

    // Start async loading if we have settings-based dirs
    if (!all_dicts || had_cached_entries) {
        startup_loading_active = TRUE;
        startup_random_word_pending = (!search_entry ||
                                       strlen(gtk_editable_get_text(GTK_EDITABLE(search_entry))) == 0);
        if (start_async_dict_loading(discover_from_dirs)) {
            if (had_cached_entries) {
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
            active_entry = all_dicts;
            populate_dict_sidebar();
        }
        webkit_web_view_load_html(web_view,
            "<h2>Welcome to Diction</h2>"
            "<p>Select a dictionary from the sidebar and start searching.</p>", "file:///");
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
                    DictEntry *e = calloc(1, sizeof(DictEntry));
                    const char *slash = strrchr(argv[1], '/');
                    const char *base = slash ? slash + 1 : argv[1];
                    e->name = strdup(base);
                    e->path = strdup(argv[1]);
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
        dict_loader_free(head);
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
    dict_loader_free(all_dicts);

    return status;
}
