#include "scan-popup.h"
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>
#include <ctype.h>

static GtkApplication *app_ref = NULL;
static AppSettings *settings_ref = NULL;
static char* (*lookup_callback)(const char *word) = NULL;

static gboolean scan_enabled = FALSE;
static GdkClipboard *primary_clipboard = NULL;
static char *last_primary_text = NULL;
static guint poll_source_id = 0;
static GtkWindow *popup_window = NULL;
static WebKitWebView *popup_webview = NULL;
static guint hide_timeout_id = 0;

/* Trim leading/trailing whitespace */
static char *trim_whitespace(char *str) {
    char *end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0)
        return str;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static gboolean hide_popup_cb(gpointer user_data) {
    (void)user_data;
    if (popup_window) {
        gtk_window_close(popup_window);
        popup_window = NULL;
    }
    hide_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void ensure_popup_window(void) {
    if (popup_window) return;

    popup_window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_decorated(popup_window, FALSE);
    gtk_window_set_deletable(popup_window, FALSE);
    gtk_window_set_focus_visible(popup_window, FALSE);
    gtk_window_set_default_size(popup_window, 400, 300);
    gtk_widget_add_css_class(GTK_WIDGET(popup_window), "osd");
    
    // Minimal WebKit setup
    popup_webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *ws = webkit_web_view_get_settings(popup_webview);
    if (settings_ref) {
        if (settings_ref->font_family)
            webkit_settings_set_default_font_family(ws, settings_ref->font_family);
        if (settings_ref->font_size > 0)
            webkit_settings_set_default_font_size(ws, settings_ref->font_size);
    }
    
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(popup_webview));
    gtk_window_set_child(popup_window, scroll);
    
    // Dismiss when focus lost
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus, "leave", G_CALLBACK(hide_popup_cb), NULL);
    gtk_widget_add_controller(GTK_WIDGET(popup_window), focus);
}

void scan_popup_show_for_word(const char *word) {
    if (!word || !*word || !lookup_callback) return;

    char *clean_word = trim_whitespace(g_strdup(word));
    if (!*clean_word) {
        g_free(clean_word);
        return;
    }

    char *html = lookup_callback(clean_word);
    g_free(clean_word);

    if (!html) return;

    ensure_popup_window();

    webkit_web_view_load_html(popup_webview, html, "file:///");
    g_free(html);

    // Position window near cursor via GdkDisplay
    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    if (seat) {
        GdkDevice *pointer = gdk_seat_get_pointer(seat);
        if (pointer) {
            double x, y;
            gdk_device_get_surface_at_position(pointer, &x, &y);
            // On Wayland, absolute positioning is restricted, but this works on X11 
            // or when using popovers. For a top-level window, it might appear in center on Wayland.
            // A more complex Wayland-specific layer-shell or xdg-popup would be needed for true absolute.
            // For now, this is best effort without wayland-specific protocols.
            // We just present it.
        }
    }

    gtk_window_present(popup_window);

    if (hide_timeout_id != 0) g_source_remove(hide_timeout_id);
    hide_timeout_id = g_timeout_add(10000, hide_popup_cb, NULL); // Auto dismiss after 10s
}

static void on_primary_clipboard_read(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data;
    GdkClipboard *clip = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clip, res, NULL);
    
    if (text && *text) {
        if (!last_primary_text || g_strcmp0(last_primary_text, text) != 0) {
            g_free(last_primary_text);
            last_primary_text = g_strdup(text);
            
            // Only trigger if enabled
            if (scan_enabled) {
                scan_popup_show_for_word(text);
            }
        }
    }
    g_free(text);
}

static gboolean poll_primary_clipboard(gpointer user_data) {
    (void)user_data;
    if (primary_clipboard && scan_enabled) {
        gdk_clipboard_read_text_async(primary_clipboard, NULL, on_primary_clipboard_read, NULL);
    }
    return G_SOURCE_CONTINUE;
}

void scan_popup_init(GtkApplication *app, AppSettings *settings,
                     char* (*lookup_cb)(const char *word)) {
    app_ref = app;
    settings_ref = settings;
    lookup_callback = lookup_cb;
    scan_enabled = settings ? settings->scan_popup_enabled : FALSE;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        primary_clipboard = gdk_display_get_primary_clipboard(display);
    }

    // Polling is required because GdkClipboard doesn't reliably emit "changed" for PRIMARY on all backends.
    int delay = settings ? settings->scan_popup_delay_ms : 500;
    if (delay < 100) delay = 100;
    poll_source_id = g_timeout_add(delay, poll_primary_clipboard, NULL);
}

void scan_popup_destroy(void) {
    if (poll_source_id != 0) {
        g_source_remove(poll_source_id);
        poll_source_id = 0;
    }
    if (hide_timeout_id != 0) {
        g_source_remove(hide_timeout_id);
        hide_timeout_id = 0;
    }
    if (popup_window) {
        gtk_window_destroy(popup_window);
        popup_window = NULL;
    }
    g_free(last_primary_text);
    last_primary_text = NULL;
    lookup_callback = NULL;
    app_ref = NULL;
    settings_ref = NULL;
}

void scan_popup_set_enabled(gboolean enabled) {
    scan_enabled = enabled;
}

gboolean scan_popup_is_enabled(void) {
    return scan_enabled;
}

void scan_popup_trigger_manual(void) {
    if (primary_clipboard && lookup_callback) {
        gdk_clipboard_read_text_async(primary_clipboard, NULL, on_primary_clipboard_read, NULL);
    }
}
