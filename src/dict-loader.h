#pragma once
#include <glib.h>
#include <gio/gio.h>
#include "dict-mmap.h"
#include <stddef.h>

#define MAX_DICTS 1000

/* Supported dictionary format types */
typedef enum {
    DICT_FORMAT_DSL,
    DICT_FORMAT_STARDICT,
    DICT_FORMAT_MDX,
    DICT_FORMAT_BGL,
    DICT_FORMAT_SLOB,
    DICT_FORMAT_XDXF,
    DICT_FORMAT_UNKNOWN
} DictFormat;

/* A loaded dictionary entry in the dictionary list */
typedef struct DictEntry {
    char *name;           /* Display name (from metadata or filename) */
    char *path;           /* Absolute path to the primary file */
    char *dict_id;        /* Cached stable dictionary id derived from path */
    DictFormat format;
    DictMmap *dict;       /* The loaded, mapped, indexed dictionary */
    gboolean has_matches; /* Set during search if this dict has matches */
    char *guessed_lang_group; /* Lazily computed volatile language group like "English->Hindi" */
    char *icon_path;
    int ref_count;        /* Reference counter for lifecycle management */
    guint32 magic;        /* 0xDEADC0DE */
    struct DictEntry *next;
} DictEntry;

/* Event types for DictLoaderCallback */
typedef enum {
    DICT_LOADER_EVENT_DISCOVERED, /* Found a potential dictionary file */
    DICT_LOADER_EVENT_STARTED,    /* Started loading/indexing header */
    DICT_LOADER_EVENT_FINISHED,   /* Successfully loaded/indexed */
    DICT_LOADER_EVENT_FAILED      /* Failed to load */
} DictLoaderEventType;

/* Callback for streaming loader: called for each dictionary found */
typedef void (*DictLoaderCallback)(DictEntry *entry, DictLoaderEventType event, void *user_data);

/* Scan a directory recursively for supported dictionary files.
 * Returns a linked list of DictEntry. Caller owns the list. */
DictEntry* dict_loader_scan_directory(const char *dirpath);

/* Streaming version: calls callback for each loaded dictionary */
void dict_loader_scan_directory_streaming(const char *dirpath, 
                                           DictLoaderCallback callback, 
                                           void *user_data,
                                           volatile gint *cancel_flag,
                                           gint expected_generation,
                                           GCancellable *cancellable);

/* Lifecycle management: ref/unref */
void dict_entry_ref(DictEntry *entry);
void dict_entry_unref(DictEntry *entry);

/* Legacy helper: Free all entries in a DictEntry linked list by unreffing each */
void dict_loader_free_list(DictEntry *head);

/* Detect format from file path */
DictFormat dict_detect_format(const char *path);

/* Load a single dictionary of any supported format
 * `cancel_flag` may be NULL. If non-NULL the loader will check the
 * cancel flag against `expected_generation` and abort early when they
 * differ. */
DictMmap* dict_load_any(const char *path, DictFormat fmt,
                        volatile gint *cancel_flag, gint expected_generation);

/**
 * Retrieve the uncompressed definition text for an entry.
 * If the dictionary is compressed, a new string is allocated and must be freed by the caller.
 * If out_to_free is non-NULL, it will be set to the pointer that needs to be freed (or NULL).
 */
const char* dict_get_definition(DictMmap *dict, const FlatTreeEntry *entry, size_t *out_len, char **out_to_free);

/* Full-text search via persistent SQLite FTS5 index.
 * Returns the first flat-index position >= start_pos whose definition
 * matches both the FTS5 pre-filter and the regex verification.
 * Returns (size_t)-1 if fts_enabled is FALSE, the index is missing,
 * or no match is found.  No full-scan fallback. */
size_t dict_search_fts(DictMmap *dict, const char *dict_path,
                       const char *query, GRegex *regex,
                       size_t start_pos, gboolean fts_enabled);
