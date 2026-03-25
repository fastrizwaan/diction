#pragma once

#include "splay-tree.h"
#include <stdbool.h>
#include <stdio.h>

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
