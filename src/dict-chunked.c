#include "dict-chunked.h"
#include <stdlib.h>
#include <string.h>

struct DictChunkWriter {
    FILE *file;
    DictCacheHeader *header;
    GArray *chunk_offsets;
    char *buffer;
    size_t buffer_pos;
    ZSTD_CCtx *cctx;
};

DictChunkWriter* dict_chunk_writer_new(FILE *file, DictCacheHeader *header) {
    DictChunkWriter *w = g_new0(DictChunkWriter, 1);
    w->file = file;
    w->header = header;
    w->chunk_offsets = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    w->buffer = g_malloc(DICT_CHUNK_SIZE);
    w->cctx = ZSTD_createCCtx();
    return w;
}

void dict_chunk_writer_set_header(DictChunkWriter *w, DictCacheHeader *h) {
    w->header = h;
}

static void flush_chunk(DictChunkWriter *w) {
    if (w->buffer_pos == 0) return;

    size_t max_comp = ZSTD_compressBound(w->buffer_pos);
    char *comp_buf = g_malloc(max_comp);
    
    size_t comp_size = ZSTD_compressCCtx(w->cctx, comp_buf, max_comp, w->buffer, w->buffer_pos, 3);
    
    uint64_t off = (uint64_t)ftell(w->file);
    g_array_append_val(w->chunk_offsets, off);
    
    fwrite(comp_buf, 1, comp_size, w->file);
    g_free(comp_buf);
    
    w->buffer_pos = 0;
}

void dict_chunk_writer_append_definition(DictChunkWriter *w, const char *data, size_t len, uint64_t *out_off) {
    *out_off = w->header->total_uncompressed_size;
    
    size_t remaining = len;
    const char *ptr = data;
    
    while (remaining > 0) {
        size_t to_copy = MIN(remaining, DICT_CHUNK_SIZE - w->buffer_pos);
        memcpy(w->buffer + w->buffer_pos, ptr, to_copy);
        w->buffer_pos += to_copy;
        w->header->total_uncompressed_size += to_copy;
        ptr += to_copy;
        remaining -= to_copy;
        
        if (w->buffer_pos == DICT_CHUNK_SIZE) {
            flush_chunk(w);
        }
    }
}

void dict_chunk_writer_finalize(DictChunkWriter *w) {
    flush_chunk(w);
    
    w->header->chunk_table_off = (uint64_t)ftell(w->file);
    w->header->chunk_count = w->chunk_offsets->len;
    
    fwrite(w->chunk_offsets->data, sizeof(uint64_t), w->chunk_offsets->len, w->file);
    
    /* Rewrite header at the beginning */
    fseek(w->file, 0, SEEK_SET);
    fwrite(w->header, sizeof(DictCacheHeader), 1, w->file);
}

void dict_chunk_writer_free(DictChunkWriter *w) {
    if (w) {
        ZSTD_freeCCtx(w->cctx);
        g_free(w->buffer);
        g_array_free(w->chunk_offsets, TRUE);
        g_free(w);
    }
}

/* Reader Implementation */

typedef struct {
    uint32_t chunk_idx;
    char *data;
} ChunkCacheEntry;

struct DictChunkReader {
    const char *mmap_data;
    size_t mmap_size;
    DictCacheHeader header;
    const uint64_t *chunk_table;
    ZSTD_DCtx *dctx;
    
    /* Simple LRU cache for 1 chunk */
    uint32_t cached_chunk_idx;
    char *cached_chunk_data;
    size_t cached_chunk_len;
    GMutex mutex;
};

DictChunkReader* dict_chunk_reader_new(const char *mmap_data, size_t mmap_size, const DictCacheHeader *header) {
    if (!mmap_data || !header) return NULL;
    if (mmap_size < sizeof(DictCacheHeader)) return NULL;
    if (header->version != DICT_CACHE_VERSION) return NULL;
    if (header->chunk_table_off > mmap_size) return NULL;
    if (header->chunk_count > 0 &&
        header->chunk_table_off + header->chunk_count * sizeof(uint64_t) > mmap_size) {
        return NULL;
    }
    
    DictChunkReader *r = g_new0(DictChunkReader, 1);
    r->mmap_data = mmap_data;
    r->mmap_size = mmap_size;
    r->header = *header;
    r->chunk_table = (const uint64_t*)(mmap_data + header->chunk_table_off);
    r->dctx = ZSTD_createDCtx();
    r->cached_chunk_idx = (uint32_t)-1;
    r->cached_chunk_data = g_malloc(DICT_CHUNK_SIZE);
    g_mutex_init(&r->mutex);
    return r;
}

char* dict_chunk_reader_get_definition(DictChunkReader *r, uint64_t offset, uint64_t len) {
    if (!r) return NULL;
    if (len == 0) return g_strdup("");
    if (offset > r->header.total_uncompressed_size ||
        len > r->header.total_uncompressed_size - offset) {
        return NULL;
    }

    char *out = g_malloc((gsize)len + 1);
    uint64_t copied = 0;

    g_mutex_lock(&r->mutex);

    while (copied < len) {
        uint64_t cur_off = offset + copied;
        uint32_t chunk_idx = (uint32_t)(cur_off / DICT_CHUNK_SIZE);
        uint32_t offset_in_chunk = (uint32_t)(cur_off % DICT_CHUNK_SIZE);

        if (chunk_idx >= r->header.chunk_count) {
            g_mutex_unlock(&r->mutex);
            g_free(out);
            return NULL;
        }

        if (r->cached_chunk_idx != chunk_idx) {
            uint64_t comp_off = r->chunk_table[chunk_idx];
            uint64_t next_off = (chunk_idx + 1 < r->header.chunk_count)
                                ? r->chunk_table[chunk_idx + 1]
                                : r->header.chunk_table_off;

            if (next_off < comp_off || comp_off > r->mmap_size || next_off > r->mmap_size) {
                g_mutex_unlock(&r->mutex);
                g_free(out);
                return NULL;
            }

            uint64_t comp_size = next_off - comp_off;
            size_t decomp_size = ZSTD_decompressDCtx(r->dctx, r->cached_chunk_data, DICT_CHUNK_SIZE,
                                                     r->mmap_data + comp_off, comp_size);
            if (ZSTD_isError(decomp_size)) {
                g_mutex_unlock(&r->mutex);
                g_free(out);
                return NULL;
            }

            r->cached_chunk_idx = chunk_idx;
            r->cached_chunk_len = decomp_size;
        }

        if (offset_in_chunk >= r->cached_chunk_len) {
            g_mutex_unlock(&r->mutex);
            g_free(out);
            return NULL;
        }

        uint64_t available = r->cached_chunk_len - offset_in_chunk;
        uint64_t to_copy = MIN(len - copied, available);
        memcpy(out + copied, r->cached_chunk_data + offset_in_chunk, (size_t)to_copy);
        copied += to_copy;
    }

    g_mutex_unlock(&r->mutex);

    out[len] = '\0';
    return out;
}

void dict_chunk_reader_free(DictChunkReader *r) {
    if (r) {
        g_mutex_clear(&r->mutex);
        ZSTD_freeDCtx(r->dctx);
        g_free(r->cached_chunk_data);
        g_free(r);
    }
}

gboolean dict_cache_is_compressed(const char *data, size_t size) {
    if (size < 4) return FALSE;
    return memcmp(data, DICT_CACHE_MAGIC, 4) == 0;
}
