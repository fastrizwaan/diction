#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>
#include <webkit/webkit.h>
#include "settings.h"

/* Initialize scan popup monitoring.
 * `scan_word_cb` is called with the selected word when a lookup should happen. */
void scan_popup_init(GtkApplication *app, AppSettings *settings,
                     void (*scan_word_cb)(const char *word));

/* Tear down scan popup resources. */
void scan_popup_destroy(void);

/* Enable or disable clipboard scanning at runtime. */
void scan_popup_set_enabled(gboolean enabled);

/* Check if scanning is currently enabled. */
gboolean scan_popup_is_enabled(void);

/* Programmatically show the scan popup for a given word (e.g. from global shortcut). */
void scan_popup_show_for_word(const char *word);

/* Manually trigger a scan of the current clipboard content. */
void scan_popup_trigger_manual(void);
