#pragma once

#include <gtk/gtk.h>

/* Initialize the StatusNotifierItem tray icon.
 * Call after the main window is created.
 * `toggle_window_cb` is called when the user clicks the tray icon.
 * `toggle_scan_cb` is called from the "Enable Scan Popup" menu item.
 * `quit_cb` is called from the "Quit" menu item. */
void tray_icon_init(GtkApplication *app, GtkWindow *main_window,
                    void (*toggle_scan_cb)(void),
                    void (*quit_cb)(void));

/* Tear down tray icon D-Bus objects. */
void tray_icon_destroy(void);

/* Update the scan-popup toggle state shown in the tray menu. */
void tray_icon_set_scan_active(gboolean active);
