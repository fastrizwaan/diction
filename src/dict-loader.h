#pragma once
#include <glib.h>
#include "dict-mmap.h"
#include <stddef.h>

/* Supported dictionary format types */
typedef enum {
    DICT_FORMAT_DSL,
    DICT_FORMAT_STARDICT,
    DICT_FORMAT_MDX,
    DICT_FORMAT_BGL,
    DICT_FORMAT_UNKNOWN
} DictFormat;

/* A loaded dictionary entry in the dictionary list */
typedef struct DictEntry {
    char *name;           /* Display name (from metadata or filename) */
    char *path;           /* Absolute path to the primary file */
    DictFormat format;
    DictMmap *dict;       /* The loaded, mapped, indexed dictionary */
    gboolean has_matches; /* Set during search if this dict has matches */
    struct DictEntry *next;
} DictEntry;

/* Callback for streaming loader: called for each dictionary found */
typedef void (*DictLoaderCallback)(DictEntry *entry, void *user_data);

/* Scan a directory recursively for supported dictionary files.
 * Returns a linked list of DictEntry. Caller owns the list. */
DictEntry* dict_loader_scan_directory(const char *dirpath);

/* Streaming version: calls callback for each loaded dictionary */
void dict_loader_scan_directory_streaming(const char *dirpath, 
                                           DictLoaderCallback callback, 
                                           void *user_data,
                                           volatile gint *cancel_flag,
                                           gint expected_generation);

/* Free all entries in a DictEntry linked list */
void dict_loader_free(DictEntry *head);

/* Detect format from file path */
DictFormat dict_detect_format(const char *path);

/* Load a single dictionary of any supported format
 * `cancel_flag` may be NULL. If non-NULL the loader will check the
 * cancel flag against `expected_generation` and abort early when they
 * differ. */
DictMmap* dict_load_any(const char *path, DictFormat fmt,
                        volatile gint *cancel_flag, gint expected_generation);
