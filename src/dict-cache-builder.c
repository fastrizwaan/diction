#include "dict-cache-builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DictCacheBuilder {
    char *cache_path;
    FILE *file;
    FILE *defs_file;
    DictCacheHeader header;
    DictCacheHeader defs_header;
    DictChunkWriter *writer;
    uint64_t headwords_len;
};

DictCacheBuilder* dict_cache_builder_new(const char *cache_path, uint64_t entry_count) {
    FILE *f = fopen(cache_path, "wb");
    if (!f) return NULL;

    DictCacheBuilder *b = g_new0(DictCacheBuilder, 1);
    b->cache_path = g_strdup(cache_path);
    b->file = f;
    b->defs_file = tmpfile();
    if (!b->defs_file) {
        fclose(f);
        g_free(b);
        return NULL;
    }

    b->header.magic[0] = 'D'; b->header.magic[1] = 'C'; b->header.magic[2] = 'M'; b->header.magic[3] = 'P';
    b->header.version = DICT_CACHE_VERSION;
    b->header.entry_count = entry_count;
    b->header.headwords_off = sizeof(DictCacheHeader);
    b->defs_header = b->header;
    
    /* Placeholder for header */
    fwrite(&b->header, sizeof(DictCacheHeader), 1, f);
    fwrite(&b->defs_header, sizeof(DictCacheHeader), 1, b->defs_file);
    
    b->writer = dict_chunk_writer_new(b->defs_file, &b->defs_header);
    
    return b;
}

void dict_cache_builder_add_headword(DictCacheBuilder *b, const char *word, size_t len, uint64_t *out_off) {
    *out_off = b->header.headwords_off + b->headwords_len;
    fwrite(word, 1, len, b->file);
    fwrite("\n", 1, 1, b->file);
    b->headwords_len += (len + 1);
}

void dict_cache_builder_add_definition(DictCacheBuilder *b, const char *data, size_t len, uint64_t *out_off) {
    dict_chunk_writer_append_definition(b->writer, data, len, out_off);
}

void dict_cache_builder_flush(DictCacheBuilder *b) {
    if (b && b->file) fflush(b->file);
}

void dict_cache_builder_finalize(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count) {
    if (!b || !b->file) return;

    b->header.entry_count = actual_count;
    b->header.headwords_len = b->headwords_len;

    dict_chunk_writer_finalize(b->writer);
    b->header.total_uncompressed_size = b->defs_header.total_uncompressed_size;
    b->header.chunk_count = b->defs_header.chunk_count;

    uint64_t chunk_base = (uint64_t)ftell(b->file);

    if (b->defs_header.chunk_table_off > sizeof(DictCacheHeader)) {
        char buf[65536];
        uint64_t remaining = b->defs_header.chunk_table_off - sizeof(DictCacheHeader);
        fseek(b->defs_file, sizeof(DictCacheHeader), SEEK_SET);
        while (remaining > 0) {
            size_t want = (remaining < sizeof(buf)) ? (size_t)remaining : sizeof(buf);
            size_t got = fread(buf, 1, want, b->defs_file);
            if (got == 0) break;
            fwrite(buf, 1, got, b->file);
            remaining -= got;
        }
    }
    
    b->header.chunk_table_off = (uint64_t)ftell(b->file);

    if (b->defs_header.chunk_count > 0) {
        uint64_t *chunk_offsets = g_new0(uint64_t, (size_t)b->defs_header.chunk_count);
        fseek(b->defs_file, (long)b->defs_header.chunk_table_off, SEEK_SET);
        if (fread(chunk_offsets, sizeof(uint64_t), (size_t)b->defs_header.chunk_count, b->defs_file) == (size_t)b->defs_header.chunk_count) {
            for (uint64_t i = 0; i < b->defs_header.chunk_count; i++) {
                if (chunk_offsets[i] >= sizeof(DictCacheHeader)) {
                    chunk_offsets[i] = chunk_base + (chunk_offsets[i] - sizeof(DictCacheHeader));
                }
            }
            fwrite(chunk_offsets, sizeof(uint64_t), (size_t)b->defs_header.chunk_count, b->file);
        }
        g_free(chunk_offsets);
    }

    uint64_t index_off = (uint64_t)ftell(b->file);
    fwrite(entries, sizeof(FlatTreeEntry), b->header.entry_count, b->file);
    
    b->header.index_off = index_off;
    
    fseek(b->file, 0, SEEK_SET);
    fwrite(&b->header, sizeof(DictCacheHeader), 1, b->file);
}

void dict_cache_builder_finalize_index_only(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count, uint32_t source_encoding, const char *stardict_sts) {
    if (!b || !b->file) return;

    b->header.entry_count = actual_count;
    b->header.headwords_len = b->headwords_len;
    b->header.source_encoding = source_encoding;

    if (stardict_sts) {
        strncpy(b->header.stardict_sts, stardict_sts, sizeof(b->header.stardict_sts) - 1);
        b->header.stardict_sts[sizeof(b->header.stardict_sts) - 1] = '\0';
    } else {
        memset(b->header.stardict_sts, 0, sizeof(b->header.stardict_sts));
    }

    /* No compressed definition chunks. */
    b->header.total_uncompressed_size = 0;
    b->header.chunk_count = 0;
    b->header.chunk_table_off = 0;

    /* Write the sorted flat tree entries directly after the headwords */
    uint64_t index_off = (uint64_t)ftell(b->file);
    fwrite(entries, sizeof(FlatTreeEntry), b->header.entry_count, b->file);
    
    b->header.index_off = index_off;
    
    /* Rewrite header */
    fseek(b->file, 0, SEEK_SET);
    fwrite(&b->header, sizeof(DictCacheHeader), 1, b->file);
}

void dict_cache_builder_free(DictCacheBuilder *b) {
    if (b) {
        dict_chunk_writer_free(b->writer);
        if (b->file) fclose(b->file);
        if (b->defs_file) fclose(b->defs_file);
        g_free(b->cache_path);
        g_free(b);
    }
}
