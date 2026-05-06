#pragma once

#include <glib.h>

typedef void (*GlobalShortcutActivatedCallback)(const char *activation_token, gpointer user_data);

void global_shortcut_setup(GlobalShortcutActivatedCallback callback, gpointer user_data);
void global_shortcut_destroy(void);
