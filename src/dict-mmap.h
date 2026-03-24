#pragma once

#include "splay-tree.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct DictMmap {
    int fd;
    FILE *tmp_file;
    const char *data;
    size_t size;
    SplayTree *index;
    char *resource_dir;
} DictMmap;

DictMmap* dict_mmap_open(const char *path);
void dict_mmap_close(DictMmap *dict);
