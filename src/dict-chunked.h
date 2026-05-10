#ifndef DICT_CHUNKED_H
#define DICT_CHUNKED_H

#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <zstd.h>

#define DICT_CACHE_MAGIC "DCMP"
#define DICT_CACHE_VERSION 3
#define DICT_CHUNK_SIZE (1024 * 1024) /* 1MB uncompressed chunks */
#define DICT_ZSTD_LEVEL 3            /* standard high-performance level */

typedef struct {
    char magic[4];
    uint32_t version;
    uint64_t entry_count;
    uint64_t headwords_off;
    uint64_t headwords_len;
    uint64_t chunk_table_off;
    uint64_t chunk_count;
    uint64_t index_off;
    uint64_t total_uncompressed_size;
    uint32_t source_encoding;
    char stardict_sts[12];
} DictCacheHeader;

/* Writer API */
typedef struct DictChunkWriter DictChunkWriter;

DictChunkWriter* dict_chunk_writer_new(FILE *file, DictCacheHeader *header);
void dict_chunk_writer_set_header(DictChunkWriter *w, DictCacheHeader *h);
void dict_chunk_writer_append_definition(DictChunkWriter *w, const char *data, size_t len, uint64_t *out_off);
void dict_chunk_writer_finalize(DictChunkWriter *w);
void dict_chunk_writer_free(DictChunkWriter *w);

/* Reader API */
typedef struct DictChunkReader DictChunkReader;

DictChunkReader* dict_chunk_reader_new(const char *mmap_data, size_t mmap_size, const DictCacheHeader *header);
char* dict_chunk_reader_get_definition(DictChunkReader *r, uint64_t offset, uint64_t len);
void dict_chunk_reader_free(DictChunkReader *r);

/* Helper to check if a file is a compressed cache */
gboolean dict_cache_is_compressed(const char *data, size_t size);

#endif /* DICT_CHUNKED_H */
