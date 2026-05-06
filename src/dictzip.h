#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct DictZip DictZip;

/* Open a .dz file for random access. Returns NULL if not a dictzip. */
DictZip* dictzip_open(const char *path);

/* Read and decompress a section of the file. */
unsigned char* dictzip_read(DictZip *dz, uint64_t offset, uint32_t length, size_t *out_len);

/* Close the dictzip. */
void dictzip_close(DictZip *dz);

/* Get total uncompressed size. */
uint64_t dictzip_get_uncompressed_size(DictZip *dz);
