#include "scan-popup.h"
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>
#include <ctype.h>

static AppSettings *settings_ref = NULL;
static void (*scan_word_callback)(const char *word) = NULL;

static gboolean scan_enabled = FALSE;
static GdkClipboard *primary_clipboard = NULL;
static GdkClipboard *regular_clipboard = NULL;
static char *last_primary_text = NULL;
static char *last_clipboard_text = NULL;
static guint poll_source_id = 0;

typedef enum {
    SCAN_READ_AUTO_PRIMARY = 0,
    SCAN_READ_AUTO_CLIPBOARD = 1,
    SCAN_READ_MANUAL_PRIMARY = 2,
    SCAN_READ_MANUAL_CLIPBOARD = 3
} ScanReadMode;

static void on_clipboard_read(GObject *source_object, GAsyncResult *res, gpointer user_data);

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

static gboolean scan_popup_try_show_for_word(const char *word) {
    if (!word || !*word || !scan_word_callback) return FALSE;

    char *word_copy = g_strdup(word);
    char *clean_word = trim_whitespace(word_copy);
    if (!*clean_word) {
        g_free(word_copy);
        return FALSE;
    }

    scan_word_callback(clean_word);
    g_free(word_copy);

    return TRUE;
}

void scan_popup_show_for_word(const char *word) {
    (void)scan_popup_try_show_for_word(word);
}

static gboolean scan_modifier_satisfied(void) {
    const char *modifier = settings_ref && settings_ref->scan_modifier_key
        ? settings_ref->scan_modifier_key
        : "none";

    if (!*modifier || g_strcmp0(modifier, "none") == 0) {
        return TRUE;
    }

    GdkDisplay *display = gdk_display_get_default();
    if (!display) {
        return FALSE;
    }

    GdkSeat *seat = gdk_display_get_default_seat(display);
    if (!seat) {
        return FALSE;
    }

    GdkModifierType state = 0;
    GdkDevice *keyboard = gdk_seat_get_keyboard(seat);
    if (keyboard) {
        state |= gdk_device_get_modifier_state(keyboard);
    }
    GdkDevice *pointer = gdk_seat_get_pointer(seat);
    if (pointer) {
        state |= gdk_device_get_modifier_state(pointer);
    }

    if (g_strcmp0(modifier, "ctrl") == 0) {
        return (state & GDK_CONTROL_MASK) != 0;
    }
    if (g_strcmp0(modifier, "alt") == 0) {
        return (state & GDK_ALT_MASK) != 0;
    }
    if (g_strcmp0(modifier, "meta") == 0) {
        return (state & (GDK_META_MASK | GDK_SUPER_MASK)) != 0;
    }

    return TRUE;
}

static gboolean scan_source_enabled(ScanReadMode mode) {
    if (!scan_enabled) {
        return FALSE;
    }

    if (mode == SCAN_READ_AUTO_PRIMARY) {
        return !settings_ref || settings_ref->scan_selection_enabled;
    }
    if (mode == SCAN_READ_AUTO_CLIPBOARD) {
        return settings_ref && settings_ref->scan_clipboard_enabled;
    }

    return TRUE;
}

static void read_clipboard_auto(GdkClipboard *clipboard, ScanReadMode mode) {
    if (clipboard && scan_word_callback && scan_source_enabled(mode) && scan_modifier_satisfied()) {
        gdk_clipboard_read_text_async(clipboard, NULL,
                                      on_clipboard_read,
                                      GINT_TO_POINTER(mode));
    }
}

static void read_regular_clipboard_manual(void) {
    if (regular_clipboard && scan_word_callback) {
        gdk_clipboard_read_text_async(regular_clipboard, NULL,
                                      on_clipboard_read, GINT_TO_POINTER(SCAN_READ_MANUAL_CLIPBOARD));
    }
}

static void on_clipboard_read(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    ScanReadMode mode = GPOINTER_TO_INT(user_data);
    GdkClipboard *clip = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clip, res, NULL);
    
    if (text && *text) {
        if ((mode == SCAN_READ_AUTO_PRIMARY || mode == SCAN_READ_AUTO_CLIPBOARD) &&
            !scan_source_enabled(mode)) {
            g_free(text);
            return;
        }

        if (mode == SCAN_READ_AUTO_PRIMARY &&
            (!last_primary_text || g_strcmp0(last_primary_text, text) != 0)) {
            g_free(last_primary_text);
            last_primary_text = g_strdup(text);
            
            // Only trigger if enabled
            if (scan_enabled) {
                (void)scan_popup_try_show_for_word(text);
            }
        } else if (mode == SCAN_READ_AUTO_CLIPBOARD &&
                   (!last_clipboard_text || g_strcmp0(last_clipboard_text, text) != 0)) {
            g_free(last_clipboard_text);
            last_clipboard_text = g_strdup(text);
            if (scan_enabled) {
                (void)scan_popup_try_show_for_word(text);
            }
        } else if (mode == SCAN_READ_MANUAL_PRIMARY) {
            if (!scan_popup_try_show_for_word(text)) {
                read_regular_clipboard_manual();
            }
        } else if (mode == SCAN_READ_MANUAL_CLIPBOARD) {
            (void)scan_popup_try_show_for_word(text);
        }
    } else if (mode == SCAN_READ_MANUAL_PRIMARY) {
        read_regular_clipboard_manual();
    }
    g_free(text);
}

static gboolean poll_primary_clipboard(gpointer user_data) {
    (void)user_data;
    read_clipboard_auto(primary_clipboard, SCAN_READ_AUTO_PRIMARY);
    read_clipboard_auto(regular_clipboard, SCAN_READ_AUTO_CLIPBOARD);
    return G_SOURCE_CONTINUE;
}

static void on_primary_clipboard_changed(GdkClipboard *clipboard, gpointer user_data) {
    (void)user_data;
    read_clipboard_auto(clipboard, SCAN_READ_AUTO_PRIMARY);
}

static void on_regular_clipboard_changed(GdkClipboard *clipboard, gpointer user_data) {
    (void)user_data;
    read_clipboard_auto(clipboard, SCAN_READ_AUTO_CLIPBOARD);
}

void scan_popup_init(GtkApplication *app, AppSettings *settings,
                     void (*scan_word_cb)(const char *word)) {
    (void)app;
    settings_ref = settings;
    scan_word_callback = scan_word_cb;
    scan_enabled = settings ? settings->scan_popup_enabled : FALSE;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        primary_clipboard = gdk_display_get_primary_clipboard(display);
        regular_clipboard = gdk_display_get_clipboard(display);
        if (primary_clipboard) {
            g_signal_connect(primary_clipboard, "changed",
                             G_CALLBACK(on_primary_clipboard_changed), NULL);
        }
        if (regular_clipboard) {
            g_signal_connect(regular_clipboard, "changed",
                             G_CALLBACK(on_regular_clipboard_changed), NULL);
        }
    }

    // Polling is kept because GdkClipboard doesn't reliably emit "changed" for PRIMARY on all backends.
    int delay = settings ? settings->scan_popup_delay_ms : 500;
    if (delay < 100) delay = 100;
    poll_source_id = g_timeout_add(delay, poll_primary_clipboard, NULL);
}

void scan_popup_destroy(void) {
    if (poll_source_id != 0) {
        g_source_remove(poll_source_id);
        poll_source_id = 0;
    }
    if (primary_clipboard) {
        g_signal_handlers_disconnect_by_func(primary_clipboard, on_primary_clipboard_changed, NULL);
    }
    if (regular_clipboard) {
        g_signal_handlers_disconnect_by_func(regular_clipboard, on_regular_clipboard_changed, NULL);
    }
    g_free(last_primary_text);
    last_primary_text = NULL;
    g_free(last_clipboard_text);
    last_clipboard_text = NULL;
    primary_clipboard = NULL;
    regular_clipboard = NULL;
    scan_word_callback = NULL;
    settings_ref = NULL;
}

void scan_popup_set_enabled(gboolean enabled) {
    scan_enabled = enabled;
}

gboolean scan_popup_is_enabled(void) {
    return scan_enabled;
}

void scan_popup_trigger_manual(void) {
    if (primary_clipboard && scan_word_callback) {
        gdk_clipboard_read_text_async(primary_clipboard, NULL,
                                      on_clipboard_read,
                                      GINT_TO_POINTER(SCAN_READ_MANUAL_PRIMARY));
    } else {
        read_regular_clipboard_manual();
    }
}
