#pragma once

#include "splay-tree.h"
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
    char *resource_dir;
} DictMmap;

DictMmap* dict_mmap_open(const char *path);
DictMmap* parse_mdx_file(const char *path);
void dict_mmap_close(DictMmap *dict);
void insert_balanced(SplayTree *t, TreeEntry *e, int start, int end);
