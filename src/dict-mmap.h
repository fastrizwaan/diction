#pragma once

#include "flat-index.h"
#include "resource-reader.h"
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

/* Re-export TreeEntry as alias for FlatTreeEntry for cache-building code */
typedef FlatTreeEntry TreeEntry;

typedef struct DictMmap {
    int fd;
    FILE *tmp_file;  // Used for temporary decompression (NULL for cached dicts)
    const char *data;
    size_t size;
    FlatIndex *index;
    struct DictChunkReader *chunk_reader;
    gboolean is_compressed;
    char *name;
    char *source_dir;
    char *resource_dir;
    char *mdx_stylesheet;
    char *source_lang;
    char *target_lang;
    char *icon_path;
    ResourceReader *resource_reader; /* lazy ZIP/MDD resource access */
} DictMmap;

/* Load/mmap a dictionary. `cancel_flag` may be NULL; if non-NULL the
 * loader should abort early when g_atomic_int_get(cancel_flag) != expected. */
DictMmap* dict_mmap_open(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_mdx_file(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_bgl_file(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_stardict(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_slob_file(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_xdxf_file(const char *path, volatile gint *cancel_flag, gint expected);
void dict_mmap_close(DictMmap *dict);
