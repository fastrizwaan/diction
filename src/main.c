#include <gtk/gtk.h>
#include <adwaita.h>
#include <webkit/webkit.h>
#include <sys/stat.h>
#include "dict-mmap.h"
#include "dict-loader.h"
#include "dsl-render.h"

static DictEntry *all_dicts = NULL;
static DictEntry *active_entry = NULL;
static WebKitWebView *web_view = NULL;
static GtkListBox *dict_listbox = NULL;

static void on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType t, gpointer user_data) {
    (void)v;
    if (t == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(d);
        WebKitNavigationAction *na = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = webkit_navigation_action_get_request(na);
        const char *uri = webkit_uri_request_get_uri(req);
        if (g_str_has_prefix(uri, "dict://")) {
            const char *word = uri + 7;
            char *unescaped = g_uri_unescape_string(word, NULL);
            gtk_editable_set_text(GTK_EDITABLE(user_data), unescaped ? unescaped : word);
            g_free(unescaped);
            webkit_policy_decision_ignore(d);
            return;
        }
    }
    webkit_policy_decision_use(d);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (strlen(query) == 0) {
        webkit_web_view_load_html(web_view, "<h2>Diction</h2><p>Start typing to search...</p>", NULL);
        return;
    }

    GString *html_res = g_string_new("<html><body style='font-family: sans-serif; padding: 10px;'>");
    int found_count = 0;

    int dict_idx = 0;
    for (DictEntry *e = all_dicts; e; e = e->next, dict_idx++) {
        if (!e->dict) continue;

        SplayNode *res = splay_tree_search(e->dict->index, query);
        if (res != NULL) {
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
                
                SplayNode *red_res = splay_tree_search(e->dict->index, link_target);
                if (red_res) {
                    def_ptr = e->dict->data + red_res->val_offset;
                    def_len = red_res->val_length;
                }
            }

            char *rendered = dsl_render_to_html(
                def_ptr, def_len,
                e->dict->data + res->key_offset, res->key_length,
                e->format, e->dict->resource_dir);
            if (rendered) {
                g_string_append_printf(html_res,
                    "<div id='dict-%d' class='dict-source' style='background: #f0f0f0; color: #555; "
                    "padding: 4px 12px; margin: 20px -10px 10px -10px; border-bottom: 1px solid #ddd; "
                    "font-size: 0.85em; font-weight: bold; text-transform: uppercase; letter-spacing: 0.05em;'>"
                    "%s</div>",
                    dict_idx, e->name);
                g_string_append(html_res, rendered);
                free(rendered);
                found_count++;
            }
        }
    }

    if (found_count > 0) {
        g_string_append(html_res, "</body></html>");
        webkit_web_view_load_html(web_view, html_res->str, "file:///");
    } else {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<div style='padding: 20px; color: #666; font-style: italic;'>"
            "No exact match for <b>%s</b> in any dictionary.</div>", query);
        webkit_web_view_load_html(web_view, buf, "file:///");
    }
    g_string_free(html_res, TRUE);
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

static void populate_dict_sidebar(void) {
    for (DictEntry *e = all_dicts; e; e = e->next) {
        GtkWidget *label = gtk_label_new(e->name);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_list_box_append(dict_listbox, label);
    }
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window), "Diction");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 650);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), main_box);

    /* Header bar with search */
    GtkWidget *header = adw_header_bar_new();
    gtk_box_append(GTK_BOX(main_box), header);

    GtkWidget *search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(search_entry, 350, -1);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), search_entry);
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);

    /* Horizontal pane: sidebar | webview */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(main_box), paned);

    /* Left: dictionary list */
    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(sidebar_scroll, 220, -1);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    dict_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(dict_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(dict_listbox, "row-selected", G_CALLBACK(on_dict_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), GTK_WIDGET(dict_listbox));
    gtk_paned_set_start_child(GTK_PANED(paned), sidebar_scroll);

    /* Right: WebKit view */
    web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    
    /* Handle internal dict:// links */
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_decide_policy), search_entry);

    GtkWidget *web_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(web_scroll, TRUE);
    gtk_widget_set_hexpand(web_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(web_scroll), GTK_WIDGET(web_view));
    gtk_paned_set_end_child(GTK_PANED(paned), web_scroll);

    gtk_paned_set_position(GTK_PANED(paned), 220);

    /* Populate sidebar */
    populate_dict_sidebar();

    /* Auto-select first dictionary */
    if (all_dicts) {
        active_entry = all_dicts;
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(dict_listbox, 0);
        if (first) gtk_list_box_select_row(dict_listbox, first);
    }

    if (all_dicts) {
        webkit_web_view_load_html(web_view,
            "<h2>Welcome to Diction</h2>"
            "<p>Select a dictionary from the sidebar and start searching.</p>", "file:///");
    } else {
        webkit_web_view_load_html(web_view,
            "<h2>No Dictionaries Found</h2>"
            "<p>Pass a dictionary directory as argument:<br>"
            "<code>./diction /path/to/dictionaries/</code></p>", "file:///");
    }

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char *argv[]) {
    /* Load dictionaries: accept a single file or a directory */
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

    AdwApplication *app = adw_application_new("org.diction.App", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    char *empty[] = { argv[0], NULL };
    int status = g_application_run(G_APPLICATION(app), 1, empty);

    g_object_unref(app);
    dict_loader_free(all_dicts);

    return status;
}
