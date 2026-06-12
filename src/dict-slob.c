#include "dict-mmap.h"
#include "dict-cache.h"
#include "flat-index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <zlib.h>
#include <zstd.h>
#include <zstd_errors.h>
#include <lzma.h>
#include <bzlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include <errno.h>
#include "settings.h"
#include "dict-cache-builder.h"
#include "dict-chunked.h"

#define SLOB_MAGIC "\x21\x2d\x31\x53\x4c\x4f\x42\x1f"

typedef enum {
    SLOB_COMP_NONE,
    SLOB_COMP_ZLIB,
    SLOB_COMP_BZ2,
    SLOB_COMP_LZMA2,
    SLOB_COMP_ZSTD,
    SLOB_COMP_UNKNOWN
} SlobCompression;

typedef struct {
    char *encoding;
    SlobCompression compression;
    uint32_t blob_count;
    uint64_t store_offset;
    uint64_t file_size;
    uint32_t refs_count;
    uint64_t refs_offset;
    uint32_t items_count;
    uint64_t items_offset;
} SlobHeader;

/* Endianness helpers (SLOB is Big Endian) */
static uint16_t read_u16be(const unsigned char *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | 
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read_u64be(const unsigned char *p) {
    return ((uint64_t)read_u32be(p) << 32) | read_u32be(p + 4);
}

static char* read_slob_text(const unsigned char **p, const unsigned char *end) {
    if (*p + 2 > end) return NULL;
    uint16_t len = read_u16be(*p);
    *p += 2;
    if (*p + len > end) return NULL;
    char *s = g_strndup((const char*)*p, len);
    *p += len;
    return s;
}

static char* read_slob_tinytext(const unsigned char **p, const unsigned char *end) {
    if (*p + 1 > end) return NULL;
    uint8_t len = **p;
    *p += 1;
    if (*p + len > end) return NULL;
    char *s = g_strndup((const char*)*p, len);
    *p += len;
    return s;
}

static unsigned char* slob_decompress(SlobCompression comp, const unsigned char *src, size_t src_len, size_t *out_len) {
    if (comp == SLOB_COMP_NONE) {
        unsigned char *dst = g_malloc(src_len);
        memcpy(dst, src, src_len);
        *out_len = src_len;
        return dst;
    }
    if (comp == SLOB_COMP_ZLIB) {
        size_t cap = src_len * 5 + 1024;
        unsigned char *dst = g_malloc(cap);
        z_stream zs = {0};
        zs.next_in = (unsigned char *)src; zs.avail_in = src_len;
        zs.next_out = dst; zs.avail_out = cap;
        if (inflateInit(&zs) != Z_OK) { g_free(dst); return NULL; }
        int ret;
        while ((ret = inflate(&zs, Z_FINISH)) == Z_BUF_ERROR && zs.avail_out == 0) {
            cap *= 2;
            dst = g_realloc(dst, cap);
            zs.next_out = dst + zs.total_out;
            zs.avail_out = cap - zs.total_out;
        }
        inflateEnd(&zs);
        if (ret != Z_STREAM_END && ret != Z_OK) { g_free(dst); return NULL; }
        *out_len = zs.total_out;
        return dst;
    }
    if (comp == SLOB_COMP_ZSTD) {
        size_t dSize = ZSTD_getFrameContentSize(src, src_len);
        if (dSize == ZSTD_CONTENTSIZE_ERROR || dSize == ZSTD_CONTENTSIZE_UNKNOWN) dSize = src_len * 5 + 1024;
        unsigned char *dst = g_malloc(dSize);
        size_t actual = ZSTD_decompress(dst, dSize, src, src_len);
        while (ZSTD_isError(actual) && ZSTD_getErrorCode(actual) == ZSTD_error_dstSize_tooSmall) {
            dSize *= 2;
            dst = g_realloc(dst, dSize);
            actual = ZSTD_decompress(dst, dSize, src, src_len);
        }
        if (ZSTD_isError(actual)) { g_free(dst); return NULL; }
        *out_len = actual;
        return dst;
    }
    if (comp == SLOB_COMP_LZMA2) {
        size_t cap = src_len * 5 + 1024;
        unsigned char *dst = g_malloc(cap);
        size_t in_pos = 0, out_pos = 0;
        uint64_t memlimit = UINT64_MAX;
        lzma_ret ret = lzma_stream_buffer_decode(&memlimit, 0, NULL, (uint8_t*)src, &in_pos, src_len, (uint8_t*)dst, &out_pos, cap);
        if (ret == LZMA_BUF_ERROR) {
            /* Retry with larger buffer for LZMA2 if needed */
            cap *= 4;
            dst = g_realloc(dst, cap);
            in_pos = 0; out_pos = 0;
            ret = lzma_stream_buffer_decode(&memlimit, 0, NULL, (uint8_t*)src, &in_pos, src_len, (uint8_t*)dst, &out_pos, cap);
        }
        if (ret != LZMA_OK) { g_free(dst); return NULL; }
        *out_len = out_pos;
        return dst;
    }
    if (comp == SLOB_COMP_BZ2) {
        unsigned int cap = (unsigned int)src_len * 5 + 1024;
        unsigned char *dst = g_malloc(cap);
        int ret = BZ2_bzBuffToBuffDecompress((char*)dst, &cap, (char*)src, (unsigned int)src_len, 0, 0);
        if (ret == BZ_OUTBUFF_FULL) {
            cap *= 4;
            dst = g_realloc(dst, cap);
            ret = BZ2_bzBuffToBuffDecompress((char*)dst, &cap, (char*)src, (unsigned int)src_len, 0, 0);
        }
        if (ret != BZ_OK) { g_free(dst); return NULL; }
        *out_len = cap;
        return dst;
    }
    return NULL;
}

typedef struct {
    DictMmap *dm;
} SlobResourceBackend;

static char* slob_res_get(ResourceReader *reader, const char *name) {
    SlobResourceBackend *backend = resource_reader_get_backend(reader);
    if (!backend || !backend->dm || !backend->dm->index) return NULL;

    const char *extract_dir = resource_reader_get_dir(reader);
    if (!extract_dir) return NULL;

    char *dest_path = g_build_filename(extract_dir, name, NULL);
    if (g_file_test(dest_path, G_FILE_TEST_EXISTS)) {
        return dest_path;
    }

    size_t pos = flat_index_search(backend->dm->index, name);
    if (pos != (size_t)-1) {
        const TreeEntry *entry = flat_index_get(backend->dm->index, pos);
        if (!entry) return NULL;
        size_t len = 0;
        char *to_free = NULL;
        const char *data = NULL;
        
        if (backend->dm->chunk_reader) {
            char *chunk_data = dict_chunk_reader_get_definition(backend->dm->chunk_reader, entry->d_off, entry->d_len);
            if (chunk_data) {
                data = chunk_data;
                len = entry->d_len;
                to_free = chunk_data;
            }
        } else {
            if (entry->d_off + entry->d_len <= backend->dm->size) {
                data = backend->dm->data + entry->d_off;
                len = entry->d_len;
            }
        }
        
        if (data && len > 0) {
            dict_cache_prepare_target_path(dest_path, len);
            FILE *f = fopen(dest_path, "wb");
            if (f) {
                fwrite(data, 1, len, f);
                fclose(f);
                g_free(to_free);
                return dest_path;
            }
        }
        g_free(to_free);
    }
    
    g_free(dest_path);
    return NULL;
}

static gboolean slob_res_has(ResourceReader *reader, const char *name) {
    SlobResourceBackend *backend = resource_reader_get_backend(reader);
    if (!backend || !backend->dm || !backend->dm->index) return FALSE;

    return flat_index_search(backend->dm->index, name) != (size_t)-1;
}

static void slob_res_close(ResourceReader *reader) {
    SlobResourceBackend *backend = resource_reader_get_backend(reader);
    g_free(backend);
}

DictMmap* parse_slob_file(const char *path, volatile gint *cancel_flag, gint expected) {
    (void)cancel_flag; (void)expected;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st_file;
    if (fstat(fd, &st_file) < 0) { close(fd); return NULL; }
    void *map = mmap(NULL, st_file.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }
    
    const unsigned char *p = map;
    const unsigned char *end = p + st_file.st_size;

    if (st_file.st_size < 8 || memcmp(p, SLOB_MAGIC, 8) != 0) {
        munmap(map, st_file.st_size); close(fd); return NULL;
    }
    p += 8; p += 16; // skip magic and UUID

    SlobHeader hdr = {0};
    hdr.encoding = read_slob_tinytext(&p, end);
    char *compr = read_slob_tinytext(&p, end);
    if (g_ascii_strcasecmp(compr, "none") == 0 || !compr || !*compr) hdr.compression = SLOB_COMP_NONE;
    else if (g_ascii_strcasecmp(compr, "zlib") == 0) hdr.compression = SLOB_COMP_ZLIB;
    else if (g_ascii_strcasecmp(compr, "bz2") == 0) hdr.compression = SLOB_COMP_BZ2;
    else if (g_ascii_strcasecmp(compr, "lzma2") == 0) hdr.compression = SLOB_COMP_LZMA2;
    else if (g_ascii_strcasecmp(compr, "zstd") == 0) hdr.compression = SLOB_COMP_ZSTD;
    else hdr.compression = SLOB_COMP_UNKNOWN;
    g_free(compr);

    uint8_t tags_count = *p++;
    char *title = NULL;
    for (int i = 0; i < tags_count; i++) {
        char *key = read_slob_tinytext(&p, end);
        char *val = read_slob_tinytext(&p, end);
        if (key && (g_ascii_strcasecmp(key, "label") == 0 || g_ascii_strcasecmp(key, "title") == 0)) {
            if (!title) title = g_strdup(val);
        }
        g_free(key); g_free(val);
    }
    uint8_t content_types_count = *p++;
    for (int i = 0; i < content_types_count; i++) {
        char *type = read_slob_text(&p, end);
        g_free(type);
    }

    if (p + 4 + 8 + 8 + 4 > end) { munmap(map, st_file.st_size); close(fd); g_free(title); return NULL; }
    hdr.blob_count = read_u32be(p); p += 4;
    hdr.store_offset = read_u64be(p); p += 8;
    hdr.file_size = read_u64be(p); p += 8;
    hdr.refs_count = read_u32be(p); p += 4;
    hdr.refs_offset = p - (unsigned char*)map;

    const unsigned char *store_p = (unsigned char*)map + hdr.store_offset;
    if (store_p + 4 > end) { munmap(map, st_file.st_size); close(fd); g_free(title); return NULL; }
    hdr.items_count = read_u32be(store_p);
    hdr.items_offset = hdr.store_offset + 4;

    char *cache_path = dict_cache_path_for(path);
    if (!dict_cache_is_valid(cache_path, path)) {
        if (!dict_cache_ensure_dir()) { g_free(cache_path); munmap(map, st_file.st_size); close(fd); g_free(title); return NULL; }
        
        /* Two-pass Cache building:
           1. Map Refs to Item/Bin.
           2. Decompress Items and write to cache, updating offsets.
        */
        if (!dict_cache_prepare_target_path(cache_path, (guint64) st_file.st_size)) {
            munmap(map, st_file.st_size);
            close(fd);
            g_free(title);
            g_free(cache_path);
            return NULL;
        }

        DictCacheBuilder *builder = dict_cache_builder_new(cache_path, hdr.refs_count);
        if (!builder) {
            munmap(map, st_file.st_size);
            close(fd);
            g_free(title);
            g_free(cache_path);
            return NULL;
        }

        typedef struct { uint32_t item_idx; uint16_t bin_idx; int64_t h_off; uint64_t h_len; } TempRef;
        TempRef *temp_refs = g_malloc_n(hdr.refs_count, sizeof(TempRef));

        uint64_t refs_block_end = hdr.refs_offset + (uint64_t)hdr.refs_count * 8;
        for (uint32_t i = 0; i < hdr.refs_count; i++) {
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                dict_cache_builder_free(builder); g_free(temp_refs); munmap(map, st_file.st_size); close(fd); g_free(title); return NULL;
            }
            if (i % 500 == 0) {
                settings_scan_progress_notify(path, (int)(i * 20 / hdr.refs_count)); /* 20% for ref scan */
            }
            uint64_t ref_ptr = read_u64be((unsigned char*)map + hdr.refs_offset + i * 8);
            const unsigned char *re_p = (unsigned char*)map + refs_block_end + ref_ptr;
            char *key = read_slob_text(&re_p, end);
            if (!key) { temp_refs[i].h_len = 0; continue; }
            uint64_t hw_off = 0;
            dict_cache_builder_add_headword(builder, key, strlen(key), &hw_off);
            temp_refs[i].h_off = hw_off;
            temp_refs[i].h_len = strlen(key);
            temp_refs[i].item_idx = read_u32be(re_p); re_p += 4;
            temp_refs[i].bin_idx = read_u16be(re_p);
            g_free(key);
        }

        /* Now decompress items and store in cache */
        uint64_t *bin_cache_offsets = g_malloc0(sizeof(uint64_t) * hdr.refs_count);
        uint32_t *bin_cache_lens = g_malloc0(sizeof(uint32_t) * hdr.refs_count);

        /* Map item_idx -> list of refs using that item */
        GList **item_to_refs = g_malloc0(sizeof(GList*) * hdr.items_count);
        for (uint32_t i = 0; i < hdr.refs_count; i++) {
            if (temp_refs[i].h_len > 0 && temp_refs[i].item_idx < hdr.items_count) {
                item_to_refs[temp_refs[i].item_idx] = g_list_prepend(item_to_refs[temp_refs[i].item_idx], GUINT_TO_POINTER(i));
            }
        }

        const unsigned char *items_offsets_p = (unsigned char*)map + hdr.items_offset;
        for (uint32_t i = 0; i < hdr.items_count; i++) {
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                g_free(item_to_refs); g_free(bin_cache_offsets); g_free(bin_cache_lens); g_free(temp_refs);
                dict_cache_builder_free(builder); unlink(cache_path); munmap(map, st_file.st_size); close(fd); g_free(title); return NULL;
            }
            if (i % 100 == 0) {
                settings_scan_progress_notify(path, 20 + (int)(i * 70 / hdr.items_count)); /* 70% for item decompression */
            }
            if (!item_to_refs[i]) continue;
            uint64_t item_ptr = read_u64be(items_offsets_p + i * 8);
            const unsigned char *it_p = (unsigned char*)map + hdr.items_offset + hdr.items_count * 8 + item_ptr;
            
            uint32_t bins_count = read_u32be(it_p); it_p += 4;
            it_p += bins_count; // skip content_ids
            uint32_t compressed_size = read_u32be(it_p); it_p += 4;
            
            size_t decomp_len = 0;
            unsigned char *decomp = slob_decompress(hdr.compression, it_p, compressed_size, &decomp_len);
            if (decomp) {
                const unsigned char *tbl = decomp;
                for (GList *l = item_to_refs[i]; l; l = l->next) {
                    uint32_t ref_idx = GPOINTER_TO_UINT(l->data);
                    uint16_t b_idx = temp_refs[ref_idx].bin_idx;
                    if (b_idx < bins_count) {
                        uint32_t b_off = read_u32be(tbl + b_idx * 4);
                        const unsigned char *b_data_p = decomp + bins_count * 4 + b_off;
                        uint32_t b_len = read_u32be(b_data_p);
                        uint64_t def_off = 0;
                        dict_cache_builder_add_definition(builder, (const char *)b_data_p + 4, b_len, &def_off);
                        bin_cache_offsets[ref_idx] = def_off;
                        bin_cache_lens[ref_idx] = b_len;
                    }
                }
                g_free(decomp);
            }
            g_list_free(item_to_refs[i]);
        }
        g_free(item_to_refs);

        TreeEntry *final_entries = g_malloc_n(hdr.refs_count, sizeof(TreeEntry));
        size_t valid_cnt = 0;
        for (uint32_t i = 0; i < hdr.refs_count; i++) {
            if (bin_cache_lens[i] > 0) {
                final_entries[valid_cnt].h_off = temp_refs[i].h_off;
                final_entries[valid_cnt].h_len = temp_refs[i].h_len;
                final_entries[valid_cnt].d_off = bin_cache_offsets[i];
                final_entries[valid_cnt].d_len = bin_cache_lens[i];
                valid_cnt++;
            }
        }
        g_free(temp_refs); g_free(bin_cache_offsets); g_free(bin_cache_lens);

        dict_cache_builder_flush(builder);
        dict_cache_builder_finalize(builder, final_entries, valid_cnt);

        /* Build SQLite headword index */
        if (valid_cnt > 0) {
            char *hw_path = dict_hw_index_path_for(path);
            int hw_fd = open(cache_path, O_RDONLY);
            if (hw_fd >= 0) {
                struct stat hw_st;
                if (fstat(hw_fd, &hw_st) == 0 && hw_st.st_size > 0) {
                    const char *hw_map = mmap(NULL, (size_t)hw_st.st_size, PROT_READ, MAP_PRIVATE, hw_fd, 0);
                    if (hw_map != MAP_FAILED) {
                        DictHwBuilder *hw = dict_hw_builder_new(hw_path);
                        if (hw) {
                            for (size_t i = 0; i < valid_cnt; i++) {
                                dict_hw_builder_add(hw,
                                    hw_map + final_entries[i].h_off,
                                    final_entries[i].h_len,
                                    final_entries[i].d_off,
                                    final_entries[i].d_len);
                            }
                            dict_hw_builder_set_metadata(hw, "source_path", path);
                            dict_hw_builder_finalize(hw);
                            struct stat hw_src_st;
                            if (stat(path, &hw_src_st) == 0) {
                                struct utimbuf times = { .actime = hw_src_st.st_mtime, .modtime = hw_src_st.st_mtime };
                                utime(hw_path, &times);
                            }
                        }
                        munmap((void*)hw_map, (size_t)hw_st.st_size);
                    }
                }
                close(hw_fd);
            }
            const char *hw_sources[] = { path };
            dict_cache_sync_mtime(hw_path, hw_sources, 1);
            g_free(hw_path);
        }
        dict_cache_builder_free(builder);
        settings_scan_progress_notify(path, 95);
        dict_cache_sync_mtime(cache_path, &path, 1);
        g_free(final_entries);
    }
    
    munmap(map, st_file.st_size); close(fd);

    char *hw_path = dict_hw_index_path_for(path);

    int cache_fd = open(cache_path, O_RDONLY);
    if (cache_fd < 0) { g_free(hw_path); g_free(cache_path); g_free(title); return NULL; }
    struct stat st_cache;
    if (fstat(cache_fd, &st_cache) < 0) { close(cache_fd); g_free(hw_path); g_free(cache_path); g_free(title); return NULL; }
    void *cache_map = mmap(NULL, st_cache.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (cache_map == MAP_FAILED) { close(cache_fd); g_free(hw_path); g_free(cache_path); g_free(title); return NULL; }

    DictMmap *dm = g_new0(DictMmap, 1);
    dm->fd = cache_fd;
    close(dm->fd);
    dm->fd = -1;
    dm->data = (const char*)cache_map;
    dm->size = st_cache.st_size;
    dm->name = title ? title : g_path_get_basename(path);
    dm->source_dir = g_path_get_dirname(path);
    dm->index = flat_index_open(hw_path);
    g_free(hw_path);
    if (dict_cache_is_compressed(dm->data, dm->size)) {
        dm->is_compressed = TRUE;
        dm->chunk_reader = dict_chunk_reader_new(dm->data, dm->size, (const DictCacheHeader*)dm->data);
    }

    char *res_dir = g_build_filename(g_get_user_cache_dir(), "diction", "slob_res", dm->name, NULL);
    SlobResourceBackend *backend = g_new0(SlobResourceBackend, 1);
    backend->dm = dm;
    dm->resource_reader = resource_reader_new(res_dir, backend, slob_res_get, slob_res_has, slob_res_close);
    g_free(res_dir);

    settings_scan_progress_notify(path, 100);
    g_free(cache_path);
    return dm;
}
