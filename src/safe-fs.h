#pragma once

#include <glib.h>
#include <stdio.h>
#include <sys/types.h>

gboolean safe_fs_is_allowed(const char *path);

int safe_unlink(const char *path);
int safe_g_unlink(const char *path);
int safe_remove(const char *path);

/* Safe wrapper for fopen. If mode contains 'w' or 'a', it checks the path. */
FILE* safe_fopen(const char *path, const char *mode);

/* Safe wrapper for open. If flags imply writing or creation, it checks the path. */
int safe_open(const char *path, int flags, ...);

#define unlink safe_unlink
#define g_unlink safe_g_unlink
#define remove safe_remove
#define fopen safe_fopen
#define open safe_open
