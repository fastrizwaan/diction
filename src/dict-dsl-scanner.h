#ifndef DICT_DSL_SCANNER_H
#define DICT_DSL_SCANNER_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <zlib.h>

typedef struct {
    gzFile gz;
    FILE *f;
    int is_compressed;
    int is_utf16le;
    int is_utf16be;
    uint64_t uncomp_offset;
    unsigned char *buf;
    size_t buf_cap;
    size_t buf_len;
    size_t buf_pos;
    int eof;
} DslScanner;

DslScanner* dsl_scanner_open(const char *path);
int dsl_scanner_read_line(DslScanner *s, char *out_utf8, size_t out_max, size_t *out_len, uint64_t *out_uncomp_offset, size_t *out_uncomp_len);
void dsl_scanner_close(DslScanner *s);

#endif
