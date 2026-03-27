#include <gtk/gtk.h>
#include <adwaita.h>
#include <webkit/webkit.h>
#include <sys/stat.h>
#include <time.h>
#include "dict-mmap.h"
#include "dict-loader.h"
#include "dict-render.h"
#include "settings.h"

static DictEntry *all_dicts = NULL;
static DictEntry *active_entry = NULL;
static WebKitWebView *web_view = NULL;
static GtkListBox *dict_listbox = NULL;
static AdwStyleManager *style_manager = NULL;
static GtkSearchEntry *search_entry = NULL;
static char *last_search_query = NULL;
static AppSettings *app_settings = NULL;
static GtkListBox *history_listbox = NULL;
static GtkListBox *related_listbox = NULL;

static void populate_dict_sidebar(void);      // forward declaration
static void start_async_dict_loading(void);   // forward declaration
static void on_search_changed(GtkSearchEntry *entry, gpointer user_data); // forward declaration
static void on_random_clicked(GtkButton *btn, gpointer user_data);

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

static void play_audio_file(const char *audio_path) {
    fprintf(stderr, "[AUDIO PLAY] Attempting to play: %s\n", audio_path);

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
    char *resource_dir = query_param_dup(query + 1, "dir");
    char *sound_file = query_param_dup(query + 1, "file");

    if (audio_url && *audio_url) {
        fprintf(stderr, "[AUDIO CLICKED] URL: %s\n", audio_url);
        play_audio_file(audio_url);
        g_free(audio_url);
        g_free(resource_dir);
        g_free(sound_file);
        return TRUE;
    }

    g_free(audio_url);

    if (!resource_dir || !sound_file) {
        g_free(resource_dir);
        g_free(sound_file);
        return FALSE;
    }

    fprintf(stderr, "[AUDIO CLICKED] Resource dir: %s\n", resource_dir);
    fprintf(stderr, "[AUDIO CLICKED] File: %s\n", sound_file);

    char *audio_path = g_build_filename(resource_dir, sound_file, NULL);
    if (g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
        play_audio_file(audio_path);
    } else {
        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path);
    }

    g_free(audio_path);
    g_free(resource_dir);
    g_free(sound_file);
    return TRUE;
}

static gboolean dict_list_filter_func(GtkListBoxRow *row, gpointer user_data) {
    (void)user_data;
    if (!search_entry) return TRUE;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (!query || strlen(query) == 0) return TRUE;

    DictEntry *e = g_object_get_data(G_OBJECT(row), "dict-entry");
    if (!e) return TRUE;
    
    return e->has_matches;
}

static void on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType t, gpointer user_data) {
    (void)v;
    if (t == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(d);
        WebKitNavigationAction *na = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = webkit_navigation_action_get_request(na);
        const char *uri = webkit_uri_request_get_uri(req);
        fprintf(stderr, "[LINK CLICKED] URI: %s\n", uri);
        if (g_str_has_prefix(uri, "dict://")) {
            const char *word = uri + 7;
            char *unescaped = g_uri_unescape_string(word, NULL);
            fprintf(stderr, "[DICT LINK] Searching for: %s\n", unescaped ? unescaped : word);
            gtk_editable_set_text(GTK_EDITABLE(user_data), unescaped ? unescaped : word);
            g_free(unescaped);
            webkit_policy_decision_ignore(d);
            return;
        } else if (g_str_has_prefix(uri, "sound://")) {
            if (!try_play_encoded_sound_uri(uri)) {
                const char *sound_file = uri + 8; // Skip "sound://"
                fprintf(stderr, "[AUDIO CLICKED] Clicked: %s\n", sound_file);
                
                /* Backward-compatible fallback */
                if (active_entry && active_entry->dict && active_entry->dict->resource_dir) {
                    char *audio_path = g_build_filename(active_entry->dict->resource_dir, sound_file, NULL);
                    if (g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
                        play_audio_file(audio_path);
                    } else {
                        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path);
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

static void on_history_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void on_related_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);

static void on_history_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    GtkWidget *label = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    if (label && GTK_IS_LABEL(label)) {
        const char *text = gtk_label_get_text(GTK_LABEL(label));
        gtk_editable_set_text(GTK_EDITABLE(search_entry), text);
    }
    gtk_list_box_unselect_all(box);
}

static void on_related_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    GtkWidget *label = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    if (label && GTK_IS_LABEL(label)) {
        const char *text = gtk_label_get_text(GTK_LABEL(label));
        gtk_editable_set_text(GTK_EDITABLE(search_entry), text);
    }
    gtk_list_box_unselect_all(box);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (last_search_query && strcmp(query, last_search_query) == 0) return;

    g_free(last_search_query);
    last_search_query = g_strdup(query);

    // Clear related listbox
    if (related_listbox) {
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(GTK_WIDGET(related_listbox)))) {
            gtk_list_box_remove(related_listbox, child);
        }
    }

    if (strlen(query) > 0 && history_listbox) {
        GtkListBoxRow *first_row = gtk_list_box_get_row_at_index(history_listbox, 0);
        gboolean is_dup = FALSE;
        if (first_row) {
            GtkWidget *label = gtk_list_box_row_get_child(first_row);
            if (label && GTK_IS_LABEL(label)) {
                if (strcmp(gtk_label_get_text(GTK_LABEL(label)), query) == 0) {
                    is_dup = TRUE;
                }
            }
        }
        if (!is_dup) {
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *label = gtk_label_new(query);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_margin_start(label, 8);
            gtk_widget_set_margin_end(label, 8);
            gtk_widget_set_margin_top(label, 6);
            gtk_widget_set_margin_bottom(label, 6);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
            gtk_list_box_prepend(history_listbox, row);
        }
    }

    if (strlen(query) > 0 && related_listbox && all_dicts && all_dicts->dict && all_dicts->dict->index) {
        // Just use the first loaded dictionary for suggestions
        SplayTree *tree = all_dicts->dict->index;
        SplayNode *res = splay_tree_search_first(tree, query);
        SplayNode *start = res ? res : tree->root;
        
        if (start) {
            size_t q_len = strlen(query);
            size_t min_len = start->key_length < q_len ? start->key_length : q_len;
            int cmp = strncasecmp(tree->mmap_data + start->key_offset, query, min_len);
            if (cmp < 0 || (cmp == 0 && start->key_length < q_len)) {
                start = splay_tree_successor(start);
            }
        }
        
        int rel_count = 0;
        while (start && rel_count < 15) {
            char *word = g_strndup(tree->mmap_data + start->key_offset, start->key_length);
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *label = gtk_label_new(word);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_margin_start(label, 8);
            gtk_widget_set_margin_end(label, 8);
            gtk_widget_set_margin_top(label, 6);
            gtk_widget_set_margin_bottom(label, 6);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
            gtk_list_box_append(related_listbox, row);
            
            g_free(word);
            start = splay_tree_successor(start);
            rel_count++;
        }
    }

    if (strlen(query) == 0) {
        webkit_web_view_load_html(web_view, "<h2>Diction</h2><p>Start typing to search...</p>", "file:///");
        // Clear matches and update filter
        for (DictEntry *e = all_dicts; e; e = e->next) e->has_matches = FALSE;
        gtk_list_box_invalidate_filter(dict_listbox);
        return;
    }

    GString *html_res = g_string_new("<html><body style='font-family: sans-serif; padding: 10px;'>");
    int found_count = 0;
    // Reset matches for all dictionaries first
    for (DictEntry *e = all_dicts; e; e = e->next) e->has_matches = FALSE;

    int dict_idx = 0;
    for (DictEntry *e = all_dicts; e; e = e->next, dict_idx++) {
        if (!e->dict) continue;

        SplayNode *res = splay_tree_search_first(e->dict->index, query);
        size_t q_len = strlen(query);
        int dict_header_shown = 0;

        while (res != NULL) {
            // Check if this node is still a match (case-insensitive and same length)
            if (res->key_length != q_len || 
                strncasecmp(e->dict->data + res->key_offset, query, q_len) != 0) {
                break;
            }

            const char *def_ptr = e->dict->data + res->val_offset;
            size_t def_len = res->val_length;

            /* Handle MDX @@@LINK= redirect */
            if (e->format == DICT_FORMAT_MDX && def_len > 8 && g_str_has_prefix(def_ptr, "@@@LINK=")) {
                char link_target[1024];
                const char *lp = def_ptr + 8;
                size_t l = 0;
                while (l < sizeof(link_target)-1 && l < (def_len - 8) && lp[l] != '\r' && lp[l] != '\n') {
                    link_target[l] = lp[l];
                    l++;
                }
                link_target[l] = '\0';

                // For redirects, we just take the first match for simplicity, 
                // or we could recursively handle it. But usually redirects are 1-to-1 or 1-to-many.
                // Here we just use the first match of the target.
                SplayNode *red_res = splay_tree_search_first(e->dict->index, link_target);
                if (red_res) {
                    def_ptr = e->dict->data + red_res->val_offset;
                    def_len = red_res->val_length;
                }
            }

            // Get current theme
            int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;

            char *rendered = dsl_render_to_html(
                def_ptr, def_len,
                e->dict->data + res->key_offset, res->key_length,
                e->format, e->dict->resource_dir, e->dict->source_dir, e->dict->mdx_stylesheet, dark_mode);
            if (rendered) {
                // Theme-aware dict source bar colors
                const char *bar_bg = dark_mode ? "#2d2d2d" : "#f0f0f0";
                const char *bar_fg = dark_mode ? "#aaaaaa" : "#555555";
                const char *bar_border = dark_mode ? "#444444" : "#dddddd";

                if (!dict_header_shown) {
                    g_string_append_printf(html_res,
                        "<div id='dict-%d' class='dict-source' style='background: %s; color: %s; "
                        "padding: 4px 12px; margin: 20px -10px 10px -10px; border-bottom: 1px solid %s; "
                        "font-size: 0.85em; font-weight: bold; text-transform: uppercase; letter-spacing: 0.05em;'>"
                        "%s</div>",
                        dict_idx, bar_bg, bar_fg, bar_border, e->name);
        
                    dict_header_shown = 1;
                    e->has_matches = TRUE;
                    found_count++;
                }

                g_string_append(html_res, rendered);
                free(rendered);
            }
            
            res = splay_tree_successor(res);
        }
    }

    if (found_count > 0) {
        g_string_append(html_res, "</body></html>");
        webkit_web_view_load_html(web_view, html_res->str, "file:///");
    } else {
        // Theme-aware no results message
        int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
        const char *text_color = dark_mode ? "#aaaaaa" : "#666666";

        char buf[512];
        snprintf(buf, sizeof(buf),
            "<div style='padding: 20px; color: %s; font-style: italic;'>"
            "No exact match for <b>%s</b> in any dictionary.</div>", text_color, query);
        webkit_web_view_load_html(web_view, buf, "file:///");
    }
    g_string_free(html_res, TRUE);

    // Update sidebar filter
    gtk_list_box_invalidate_filter(dict_listbox);
}

static void on_random_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (!all_dicts) return;

    // Count dicts
    int count = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) if (e->dict && e->dict->index->root) count++;
    if (count == 0) return;

    // Pick random dict
    int target = rand() % count;
    DictEntry *e = all_dicts;
    int cur = 0;
    while (e) {
        if (e->dict && e->dict->index->root) {
            if (cur == target) break;
            cur++;
        }
        e = e->next;
    }

    if (e && e->dict && e->dict->index->root) {
        SplayNode *node = splay_tree_get_random(e->dict->index);
        if (node) {
            const char *word = e->dict->data + node->key_offset;
            size_t len = node->key_length;
            char *word_str = g_strndup(word, len);
            gtk_editable_set_text(GTK_EDITABLE(search_entry), word_str);
            g_free(word_str);
            // Search will be triggered by "search-changed" signal
        }
    }
}

static void on_dict_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    int idx = gtk_list_box_row_get_index(row);

    char js[256];
    snprintf(js, sizeof(js),
        "var el = document.getElementById('dict-%d'); "
        "if (el) { el.scrollIntoView({behavior: 'smooth', block: 'start'}); }",
        idx);
    webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);

    DictEntry *e = all_dicts;
    for (int i = 0; i < idx && e; i++) e = e->next;
    if (e && e->dict) {
        active_entry = e;
    }
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

    // Re-run search with new theme
    g_clear_pointer(&last_search_query, g_free);
    on_search_changed(search_entry, NULL);
}

// Theme change handler
static void on_theme_changed(AdwStyleManager *manager, GParamSpec *pspec, gpointer user_data) {
    (void)manager; (void)pspec; (void)user_data;

    // Update webview background color
    GdkRGBA bg_color;
    int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;

    if (dark_mode) {
        gdk_rgba_parse(&bg_color, "#1e1e1e");
    } else {
        gdk_rgba_parse(&bg_color, "#ffffff");
    }
    webkit_web_view_set_background_color(web_view, &bg_color);

    // Refresh current content
    refresh_search_results();
}

static void reload_dictionaries_from_settings(void *user_data) {
    (void)user_data;
    // Free existing dicts
    dict_loader_free(all_dicts);
    all_dicts = NULL;
    active_entry = NULL;

    // Clear sidebar
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(dict_listbox))))
        gtk_list_box_remove(dict_listbox, child);

    // Show "Reloading..." and start async scan
    webkit_web_view_load_html(web_view,
        "<html><body style='font-family: sans-serif; text-align: center; margin-top: 3em; opacity: 0.6;'>"
        "<h2>Reloading dictionaries\u2026</h2><p>Please wait.</p>"
        "</body></html>", "file:///");

    start_async_dict_loading();
}

static void show_settings_dialog(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(user_data));
    if (window) {
        GtkWidget *dialog = settings_dialog_new(window, app_settings, style_manager,
            reload_dictionaries_from_settings, NULL);
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

static void append_entry_to_sidebar(DictEntry *e) {
    fprintf(stderr, "[SIDEBAR] Adding dictionary: '%s' (format: %d)\n", e->name, e->format);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = gtk_label_new(e->name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);
    gtk_widget_set_margin_top(label, 4);
    gtk_widget_set_margin_bottom(label, 4);
    
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    g_object_set_data(G_OBJECT(row), "dict-entry", e);
    gtk_list_box_append(dict_listbox, row);
}

static void populate_dict_sidebar(void) {
    for (DictEntry *e = all_dicts; e; e = e->next)
        append_entry_to_sidebar(e);
}

// ------- Async loading infrastructure -------
typedef struct {
    char **dirs;    // NULL-terminated array of directory paths to scan
    int   n_dirs;
} LoadThreadArgs;

// Payload passed from thread to main thread via g_idle_add
typedef struct {
    DictEntry *entry; // single loaded entry (next == NULL on delivery)
    gboolean   done;  // TRUE = loading finished
} LoadIdleData;

static gboolean on_dict_loaded_idle(gpointer user_data) {
    LoadIdleData *ld = user_data;

    if (!ld->done && ld->entry) {
        DictEntry *e = ld->entry;
        e->next = NULL;

        // Check for duplicate in global list (might exist if reload/re-scan happened)
        DictEntry *existing = NULL;
        for (DictEntry *curr = all_dicts; curr; curr = curr->next) {
            if (curr->path && strcmp(curr->path, e->path) == 0) {
                existing = curr;
                break;
            }
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
            // Add to sidebar
            append_entry_to_sidebar(e);
        }

        // Auto-select the very first dictionary
        if (all_dicts == e && !active_entry) {
            active_entry = e;
            GtkListBoxRow *first = gtk_list_box_get_row_at_index(dict_listbox, 0);
            if (first) gtk_list_box_select_row(dict_listbox, first);
        }
    }

    if (ld->done) {
        // Loading complete — update welcome page if no dicts found
        if (!all_dicts) {
            webkit_web_view_load_html(web_view,
                "<h2>No Dictionaries Found</h2>"
                "<p>Open <b>Preferences</b> and add a dictionary directory.</p>",
                "file:///");
        } else {
            // Do a random search on startup if nothing is there
            const char *current = gtk_editable_get_text(GTK_EDITABLE(search_entry));
            if (strlen(current) == 0) {
                on_random_clicked(NULL, NULL);
            }
        }
    }

    g_free(ld);
    return G_SOURCE_REMOVE;
}

static void on_dict_found_streaming(DictEntry *e, void *user_data) {
    (void)user_data;
    LoadIdleData *ld = g_new0(LoadIdleData, 1);
    ld->entry = e;
    ld->done  = FALSE;
    g_idle_add(on_dict_loaded_idle, ld);
}

static gpointer dict_load_thread(gpointer user_data) {
    LoadThreadArgs *args = user_data;

    for (int i = 0; i < args->n_dirs; i++) {
        dict_loader_scan_directory_streaming(args->dirs[i], on_dict_found_streaming, NULL);
    }

    // Signal completion
    LoadIdleData *done_ld = g_new0(LoadIdleData, 1);
    done_ld->done = TRUE;
    g_idle_add(on_dict_loaded_idle, done_ld);

    // Free args
    for (int i = 0; i < args->n_dirs; i++)
        g_free(args->dirs[i]);
    g_free(args->dirs);
    g_free(args);
    return NULL;
}

static void start_async_dict_loading(void) {
    if (!app_settings || app_settings->dictionary_dirs->len == 0)
        return;

    LoadThreadArgs *args = g_new0(LoadThreadArgs, 1);
    args->n_dirs = (int)app_settings->dictionary_dirs->len;
    args->dirs   = g_new(char *, args->n_dirs + 1);
    for (int i = 0; i < args->n_dirs; i++)
        args->dirs[i] = g_strdup(g_ptr_array_index(app_settings->dictionary_dirs, i));
    args->dirs[args->n_dirs] = NULL;

    GThread *thread = g_thread_new("dict-loader", dict_load_thread, args);
    g_thread_unref(thread); // fire-and-forget
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window), "Diction");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 650);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), main_box);

    AdwOverlaySplitView *split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    gtk_widget_set_vexpand(GTK_WIDGET(split_view), TRUE);
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(split_view));

    /* --- Sidebar --- */
    GtkWidget *sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Sidebar Header */
    GtkWidget *sidebar_header = adw_header_bar_new();
    gtk_widget_add_css_class(sidebar_header, "flat");
    GtkWidget *title_label = gtk_label_new("Diction");
    gtk_widget_add_css_class(title_label, "title");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(sidebar_header), title_label);

    GtkWidget *random_btn = gtk_button_new_from_icon_name("media-playlist-shuffle-symbolic");
    gtk_widget_add_css_class(random_btn, "flat");
    gtk_widget_set_tooltip_text(random_btn, "Random Headword");
    g_signal_connect(random_btn, "clicked", G_CALLBACK(on_random_clicked), NULL);
    adw_header_bar_pack_start(ADW_HEADER_BAR(sidebar_header), random_btn);

    GtkWidget *settings_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(settings_btn), "open-menu-symbolic");
    gtk_widget_add_css_class(settings_btn, "flat");
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Preferences", "app.settings");
    g_menu_append(menu, "About", "app.about");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(settings_btn), G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(sidebar_header), settings_btn);

    gtk_box_append(GTK_BOX(sidebar_vbox), sidebar_header);

    /* Sidebar Stack */
    AdwViewStack *sidebar_stack = ADW_VIEW_STACK(adw_view_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(sidebar_stack), TRUE);

    /* Search/Related Tab */
    GtkWidget *related_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(related_scroll, TRUE);
    gtk_widget_set_hexpand(related_scroll, TRUE);
    related_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(related_listbox), "navigation-sidebar");
    gtk_list_box_set_selection_mode(related_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(related_listbox, "row-selected", G_CALLBACK(on_related_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(related_scroll), GTK_WIDGET(related_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, related_scroll, "search", "Search", "system-search-symbolic");

    /* Dictionaries Tab */
    GtkWidget *dict_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(dict_scroll, TRUE);
    gtk_widget_set_hexpand(dict_scroll, TRUE);
    dict_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(dict_listbox), "navigation-sidebar");
    gtk_list_box_set_filter_func(dict_listbox, dict_list_filter_func, NULL, NULL);
    gtk_list_box_set_selection_mode(dict_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(dict_listbox, "row-selected", G_CALLBACK(on_dict_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dict_scroll), GTK_WIDGET(dict_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, dict_scroll, "dictionaries", "Dictionaries", "accessories-dictionary-symbolic");

    /* History Tab */
    GtkWidget *history_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(history_scroll, TRUE);
    gtk_widget_set_hexpand(history_scroll, TRUE);
    history_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(history_listbox), "navigation-sidebar");
    gtk_list_box_set_selection_mode(history_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(history_listbox, "row-selected", G_CALLBACK(on_history_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(history_scroll), GTK_WIDGET(history_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, history_scroll, "history", "History", "document-open-recent-symbolic");

    /* Placeholder Tabs to match Python UI */
    GtkWidget *fav_label = gtk_label_new("Favorites (Not implemented)");
    adw_view_stack_add_titled_with_icon(sidebar_stack, fav_label, "favorites", "Favorites", "starred-symbolic");
    
    GtkWidget *groups_label = gtk_label_new("Groups (Not implemented)");
    adw_view_stack_add_titled_with_icon(sidebar_stack, groups_label, "groups", "Groups", "folder-symbolic");

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
    gtk_box_append(GTK_BOX(sidebar_vbox), tabs_box);

    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_view), sidebar_vbox);

    /* --- Content --- */
    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    
    GtkWidget *content_header = adw_header_bar_new();
    gtk_widget_add_css_class(content_header, "content-header");
    adw_toolbar_view_add_top_bar(toolbar_view, content_header);

    /* Sidebar toggle */
    GtkWidget *sidebar_toggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebar_toggle), "sidebar-show-symbolic");
    g_object_bind_property(split_view, "show-sidebar", sidebar_toggle, "active", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), sidebar_toggle);

    search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_size_request(GTK_WIDGET(search_entry), 350, -1);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(content_header), GTK_WIDGET(search_entry));
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);

    /* WebKit view */
    web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *web_settings = webkit_web_view_get_settings(web_view);
    webkit_settings_set_auto_load_images(web_settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(web_settings, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(web_settings, TRUE);

    /* Handle internal dict:// links */
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_decide_policy), search_entry);

    GtkWidget *web_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(web_scroll, TRUE);
    gtk_widget_set_hexpand(web_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(web_scroll), GTK_WIDGET(web_view));
    
    adw_toolbar_view_set_content(toolbar_view, web_scroll);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_view), GTK_WIDGET(toolbar_view));

    /* Populate sidebar */
    populate_dict_sidebar();

    /* Auto-select first dictionary */
    if (all_dicts) {
        active_entry = all_dicts;
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(dict_listbox, 0);
        if (first) gtk_list_box_select_row(dict_listbox, first);
    }

    // Initialize style manager for theme support
    style_manager = adw_style_manager_get_default();
    g_signal_connect(style_manager, "notify::dark", G_CALLBACK(on_theme_changed), NULL);

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
    );
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Apply saved theme preference
    if (app_settings && app_settings->theme) {
        if (strcmp(app_settings->theme, "light") == 0) {
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
        } else if (strcmp(app_settings->theme, "dark") == 0) {
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
        } else {
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_DEFAULT);
        }
    }

    // Apply initial theme to webview
    GdkRGBA bg_color;
    int dark_mode = adw_style_manager_get_dark(style_manager) ? 1 : 0;
    if (dark_mode) {
        gdk_rgba_parse(&bg_color, "#1e1e1e");
    } else {
        gdk_rgba_parse(&bg_color, "#ffffff");
    }
    webkit_web_view_set_background_color(web_view, &bg_color);

    /* Show the window FIRST, then start background loading */
    webkit_web_view_load_html(web_view,
        "<html><body style='font-family: sans-serif; text-align: center; margin-top: 3em; opacity: 0.6;'>"
        "<h2>Loading dictionaries…</h2><p>Please wait.</p>"
        "</body></html>", "file:///");

    gtk_window_present(GTK_WINDOW(window));

    // Start async loading if we have settings-based dirs
    if (!all_dicts) {
        start_async_dict_loading();
    } else {
        // CLI-mode: dicts already loaded synchronously, just populate
        populate_dict_sidebar();
        if (all_dicts) {
            active_entry = all_dicts;
            GtkListBoxRow *first = gtk_list_box_get_row_at_index(dict_listbox, 0);
            if (first) gtk_list_box_select_row(dict_listbox, first);
        }
        webkit_web_view_load_html(web_view,
            "<h2>Welcome to Diction</h2>"
            "<p>Select a dictionary from the sidebar and start searching.</p>", "file:///");
    }
}

int main(int argc, char *argv[]) {
    // Disable compositing to fix rendering issues
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
    // Seed random number generator
    srand(time(NULL));
    // Load settings first
    app_settings = settings_load();

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
                DictMmap *d = dict_load_any(argv[1], fmt);
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

    AdwApplication *app = adw_application_new("org.diction.App", G_APPLICATION_DEFAULT_FLAGS);

    // Add settings and about actions
    GSimpleAction *settings_action = g_simple_action_new("settings", NULL);
    g_signal_connect(settings_action, "activate", G_CALLBACK(show_settings_dialog), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(settings_action));
    g_object_unref(settings_action);

    GSimpleAction *about_action = g_simple_action_new("about", NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(show_about_dialog), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));
    g_object_unref(about_action);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    char *empty[] = { argv[0], NULL };
    int status = g_application_run(G_APPLICATION(app), 1, empty);

    // Save settings on exit
    if (app_settings) {
        settings_save(app_settings);
        settings_free(app_settings);
    }

    g_object_unref(app);
    dict_loader_free(all_dicts);

    return status;
}
