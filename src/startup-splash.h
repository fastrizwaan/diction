#pragma once

#include <gtk/gtk.h>

void startup_splash_show(GtkApplication *app, GtkWindow *parent, GCallback cancel_callback);
void startup_splash_update_progress(guint completed, guint total, const char *status_text);
void startup_splash_close(void);
gboolean startup_splash_is_active(void);
