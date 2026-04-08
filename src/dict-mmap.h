#pragma once

#include "splay-tree.h"
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

typedef struct {
    int64_t h_off;
    uint64_t h_len;
    int64_t d_off;
    uint64_t d_len;
} TreeEntry;

typedef struct DictMmap {
    int fd;
    FILE *tmp_file;  // Used for temporary decompression (NULL for cached dicts)
    const char *data;
    size_t size;
    SplayTree *index;
    char *name;
    char *source_dir;
    char *resource_dir;
    char *mdx_stylesheet;
} DictMmap;

/* Load/mmap a dictionary. `cancel_flag` may be NULL; if non-NULL the
 * loader should abort early when g_atomic_int_get(cancel_flag) != expected. */
DictMmap* dict_mmap_open(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_mdx_file(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_bgl_file(const char *path, volatile gint *cancel_flag, gint expected);
DictMmap* parse_stardict(const char *path, volatile gint *cancel_flag, gint expected);
void dict_mmap_close(DictMmap *dict);
void insert_balanced(SplayTree *t, TreeEntry *e, int start, int end);
