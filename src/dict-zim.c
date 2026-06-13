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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include "settings.h"
#include "dict-cache-builder.h"
#include "dict-chunked.h"

#define ZIM_MAGIC_V5 0x044D495A
#define ZIM_MAGIC_V6 0x054D495A

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t uuid[16];
    uint32_t article_count;
    uint32_t cluster_count;
    uint64_t url_ptr_pos;
    uint64_t title_ptr_pos;
    uint64_t cluster_ptr_pos;
    uint64_t mime_list_pos;
    uint32_t main_page;
    uint32_t layout_page;
    uint64_t checksum_pos;
} ZimHeader;

static uint16_t read_u16le(const unsigned char *p) {
    return ((uint16_t)p[1] << 8) | p[0];
}

static uint32_t read_u32le(const unsigned char *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | 
           ((uint32_t)p[1] << 8) | p[0];
}

static uint64_t read_u64le(const unsigned char *p) {
    return ((uint64_t)read_u32le(p + 4) << 32) | read_u32le(p);
}

static unsigned char* zim_decompress(uint8_t comp, const unsigned char *src, size_t src_len, size_t *out_len) {
    if (comp == 0 || comp == 1) { // None
        unsigned char *dst = g_malloc(src_len);
        memcpy(dst, src, src_len);
        *out_len = src_len;
        return dst;
    }
    if (comp == 5) { // Zstd
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
    if (comp == 4) { // LZMA
        size_t cap = src_len * 20 + 1024;
        unsigned char *dst = g_malloc(cap);
        lzma_stream strm = LZMA_STREAM_INIT;
        lzma_ret ret = lzma_auto_decoder(&strm, UINT64_MAX, 0);
        if (ret != LZMA_OK) { g_free(dst); return NULL; }
        strm.next_in = src;
        strm.avail_in = src_len;
        strm.next_out = dst;
        strm.avail_out = cap;
        ret = lzma_code(&strm, LZMA_FINISH);
        while ((ret == LZMA_OK || ret == LZMA_BUF_ERROR) && strm.avail_out == 0) {
            cap *= 2;
            dst = g_realloc(dst, cap);
            strm.next_out = dst + strm.total_out;
            strm.avail_out = cap - strm.total_out;
            ret = lzma_code(&strm, LZMA_FINISH);
        }
        lzma_end(&strm);
        if (ret != LZMA_STREAM_END && ret != LZMA_OK) { g_free(dst); return NULL; }
        *out_len = strm.total_out;
        return dst;
    }
    return NULL;
}

/* Parse the MIME type list at mime_list_pos. Returns a NULL-terminated array. */
static char** zim_parse_mime_list(const unsigned char *data, uint64_t mime_list_pos, size_t file_size, int *out_count) {
    GPtrArray *arr = g_ptr_array_new();
    const unsigned char *p = data + mime_list_pos;
    const unsigned char *end = data + file_size;
    while (p < end) {
        const char *s = (const char *)p;
        size_t slen = strnlen(s, end - p);
        if (slen == 0) break;
        g_ptr_array_add(arr, g_strdup(s));
        p += slen + 1;
    }
    if (out_count) *out_count = (int)arr->len;
    g_ptr_array_add(arr, NULL);
    return (char **)g_ptr_array_free(arr, FALSE);
}

static gboolean zim_is_article_mime(const char *mime) {
    return mime && strcmp(mime, "text/html") == 0;
}

/* Read a ZIM metadata entry's blob data as a string. Returns newly allocated. */
static char* zim_read_metadata_entry(const unsigned char *file_data, size_t file_size,
                                      const ZimHeader *hdr, const char *meta_key) {
    for (uint32_t i = 0; i < hdr->article_count; i++) {
        if (hdr->url_ptr_pos + (uint64_t)i * 8 + 8 > file_size) break;
        uint64_t dir_off = read_u64le(file_data + hdr->url_ptr_pos + i * 8);
        if (dir_off + 12 > file_size) continue;
        const unsigned char *dp = file_data + dir_off;
        uint16_t mt = read_u16le(dp);
        if (mt == 0xffff) continue;
        char ns = (char)dp[3];
        if (ns != 'M') continue;
        /* Skip to URL field: mimetype(2) + param_len(1) + namespace(1) + revision(4) + cluster(4) + blob(4) = 16 */
        const unsigned char *url_p = dp + 16;
        if (url_p >= file_data + file_size) continue;
        const char *url = (const char *)url_p;
        if (strcmp(url, meta_key) != 0) continue;
        /* Found it — read blob from cluster */
        uint32_t cluster_idx = read_u32le(dp + 8);
        uint32_t blob_idx = read_u32le(dp + 12);
        if (cluster_idx >= hdr->cluster_count) return NULL;
        uint64_t c_start = read_u64le(file_data + hdr->cluster_ptr_pos + (uint64_t)cluster_idx * 8);
        uint64_t c_end = (cluster_idx + 1 < hdr->cluster_count)
            ? read_u64le(file_data + hdr->cluster_ptr_pos + (uint64_t)(cluster_idx + 1) * 8)
            : hdr->checksum_pos;
        if (c_start >= file_size || c_end > file_size || c_start >= c_end) return NULL;
        const unsigned char *c_p = file_data + c_start;
        uint8_t comp_type = *c_p & 0x0f;
        gboolean extended = (*c_p & 0x10) != 0;
        size_t decomp_len = 0;
        unsigned char *decomp = zim_decompress(comp_type, c_p + 1, c_end - c_start - 1, &decomp_len);
        if (!decomp) return NULL;
        int offset_size = extended ? 8 : 4;
        uint64_t first_off = extended ? read_u64le(decomp) : read_u32le(decomp);
        uint32_t blob_count = (uint32_t)(first_off / offset_size);
        if (blob_count > 0) blob_count--;
        char *result = NULL;
        if (blob_idx < blob_count) {
            uint64_t b_off = extended ? read_u64le(decomp + blob_idx * offset_size) : read_u32le(decomp + blob_idx * offset_size);
            uint64_t b_end = extended ? read_u64le(decomp + (blob_idx + 1) * offset_size) : read_u32le(decomp + (blob_idx + 1) * offset_size);
            if (b_end >= b_off && b_end <= decomp_len) {
                result = g_strndup((const char *)decomp + b_off, b_end - b_off);
            }
        }
        g_free(decomp);
        return result;
    }
    return NULL;
}

typedef struct {
    DictMmap *dm;
} ZimResourceBackend;

static char* zim_res_get(ResourceReader *reader, const char *name) {
    ZimResourceBackend *backend = resource_reader_get_backend(reader);
    if (!backend || !backend->dm || !backend->dm->index) return NULL;

    const char *extract_dir = resource_reader_get_dir(reader);
    if (!extract_dir) return NULL;

    char *dest_path = g_build_filename(extract_dir, name, NULL);
    if (g_file_test(dest_path, G_FILE_TEST_EXISTS)) return dest_path;

    char *res_name = g_strdup_printf("\x01%s", name);
    size_t pos = flat_index_search(backend->dm->index, res_name);
    g_free(res_name);
    if (pos == (size_t)-1) {
        pos = flat_index_search(backend->dm->index, name);
    }
    
    if (pos != (size_t)-1) {
        const TreeEntry *entry = flat_index_get(backend->dm->index, pos);
        if (!entry) return NULL;
        size_t len = 0;
        char *to_free = NULL;
        const char *data = NULL;
        if (backend->dm->chunk_reader) {
            char *chunk_data = dict_chunk_reader_get_definition(backend->dm->chunk_reader, entry->d_off, entry->d_len);
            if (chunk_data) { data = chunk_data; len = entry->d_len; to_free = chunk_data; }
        } else {
            if (entry->d_off + entry->d_len <= backend->dm->size) {
                data = backend->dm->data + entry->d_off;
                len = entry->d_len;
            }
        }
        if (data && len > 0) {
            dict_cache_prepare_target_path(dest_path, len);
            FILE *f = fopen(dest_path, "wb");
            if (f) { fwrite(data, 1, len, f); fclose(f); g_free(to_free); return dest_path; }
        }
        g_free(to_free);
    }
    g_free(dest_path);
    return NULL;
}

static gboolean zim_res_has(ResourceReader *reader, const char *name) {
    ZimResourceBackend *backend = resource_reader_get_backend(reader);
    if (!backend || !backend->dm || !backend->dm->index) return FALSE;
    char *res_name = g_strdup_printf("\x01%s", name);
    size_t pos = flat_index_search(backend->dm->index, res_name);
    g_free(res_name);
    if (pos != (size_t)-1) return TRUE;
    return flat_index_search(backend->dm->index, name) != (size_t)-1;
}

static void zim_res_close(ResourceReader *reader) {
    ZimResourceBackend *backend = resource_reader_get_backend(reader);
    g_free(backend);
}

DictMmap* parse_zim_file(const char *path, volatile gint *cancel_flag, gint expected) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st_file;
    if (fstat(fd, &st_file) < 0) { close(fd); return NULL; }
    void *map = mmap(NULL, st_file.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }
    
    const unsigned char *p = map;

    if (st_file.st_size < 80) { munmap(map, st_file.st_size); close(fd); return NULL; }
    
    uint32_t magic = read_u32le(p);
    if (magic != ZIM_MAGIC_V5 && magic != ZIM_MAGIC_V6) {
        munmap(map, st_file.st_size); close(fd); return NULL;
    }
    
    ZimHeader hdr = {0};
    hdr.magic = magic;
    hdr.version = read_u32le(p + 4);
    memcpy(hdr.uuid, p + 8, 16);
    hdr.article_count = read_u32le(p + 24);
    hdr.cluster_count = read_u32le(p + 28);
    hdr.url_ptr_pos = read_u64le(p + 32);
    hdr.title_ptr_pos = read_u64le(p + 40);
    hdr.cluster_ptr_pos = read_u64le(p + 48);
    hdr.mime_list_pos = read_u64le(p + 56);
    hdr.main_page = read_u32le(p + 64);
    hdr.layout_page = read_u32le(p + 68);
    hdr.checksum_pos = read_u64le(p + 72);
    
    char *title = g_strdup(g_path_get_basename(path));

    /* Parse MIME type list */
    int mime_count = 0;
    char **mime_types = zim_parse_mime_list(p, hdr.mime_list_pos, st_file.st_size, &mime_count);

    /* Try to extract metadata title before cache build */
    char *meta_title = zim_read_metadata_entry(p, st_file.st_size, &hdr, "Title");
    if (meta_title && *meta_title) {
        g_free(title);
        title = meta_title;
    } else {
        g_free(meta_title);
    }

    char *cache_path = dict_cache_path_for(path);
    if (!dict_cache_is_valid(cache_path, path)) {
        if (!dict_cache_ensure_dir()) { g_free(cache_path); munmap(map, st_file.st_size); close(fd); g_free(title); g_strfreev(mime_types); return NULL; }
        
        if (!dict_cache_prepare_target_path(cache_path, (guint64) st_file.st_size)) {
            munmap(map, st_file.st_size); close(fd); g_free(title); g_free(cache_path); g_strfreev(mime_types); return NULL;
        }

        DictCacheBuilder *builder = dict_cache_builder_new(cache_path, hdr.article_count);
        if (!builder) {
            munmap(map, st_file.st_size); close(fd); g_free(title); g_free(cache_path); g_strfreev(mime_types); return NULL;
        }

        typedef struct { uint32_t cluster_idx; uint32_t blob_idx; int64_t h_off; uint64_t h_len; } TempRef;
        TempRef *temp_refs = g_malloc_n(hdr.article_count, sizeof(TempRef));
        uint32_t valid_refs = 0;

        /* Also track redirects for resolution */
        typedef struct { uint32_t redirect_target; int64_t h_off; uint64_t h_len; } RedirectRef;
        RedirectRef *redir_refs = g_malloc_n(hdr.article_count, sizeof(RedirectRef));
        uint32_t redir_count = 0;

        for (uint32_t i = 0; i < hdr.article_count; i++) {
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                dict_cache_builder_free(builder); g_free(temp_refs); g_free(redir_refs); munmap(map, st_file.st_size); close(fd); g_free(title); g_strfreev(mime_types); return NULL;
            }
            if (i % 1000 == 0) settings_scan_progress_notify(path, (int)(i * 20 / hdr.article_count));
            
            uint64_t dir_offset = read_u64le(p + hdr.url_ptr_pos + i * 8);
            const unsigned char *dir_p = p + dir_offset;
            
            uint16_t mimetype = read_u16le(dir_p); dir_p += 2;
            uint8_t param_len = *dir_p++;
            char namespace = (char)*dir_p++;
            uint32_t revision = read_u32le(dir_p); dir_p += 4;
            (void)param_len; (void)revision;
            
            if (mimetype == 0xffff) {
                /* Redirect entry — record for later resolution */
                uint32_t redir_target = read_u32le(dir_p); dir_p += 4;
                const char *url = (const char*)dir_p;
                dir_p += strlen(url) + 1;
                const char *entry_title = (const char*)dir_p;

                /* Only index redirects from article namespaces */
                if (namespace == 'A' || namespace == 'C') {
                    const char *headword = (strlen(entry_title) > 0) ? entry_title : url;
                    uint64_t hw_off = 0;
                    dict_cache_builder_add_headword(builder, headword, strlen(headword), &hw_off);
                    redir_refs[redir_count].h_off = hw_off;
                    redir_refs[redir_count].h_len = strlen(headword);
                    redir_refs[redir_count].redirect_target = redir_target;
                    redir_count++;
                }
            } else if (mimetype != 0xfffe && mimetype != 0xfffd) {
                uint32_t cluster_idx = read_u32le(dir_p); dir_p += 4;
                uint32_t blob_idx = read_u32le(dir_p); dir_p += 4;
                
                // Read URL
                const char *url = (const char*)dir_p;
                dir_p += strlen(url) + 1;
                // Read Title
                const char *entry_title = (const char*)dir_p;
                dir_p += strlen(entry_title) + 1;

                /* Resolve MIME type string */
                const char *mime_str = (mimetype < mime_count) ? mime_types[mimetype] : NULL;
                
                char hw_buf[1024];
                const char *headword;

                if (namespace == 'A' || (namespace == 'C' && zim_is_article_mime(mime_str))) {
                    /* Article entry — visible in search */
                    headword = (strlen(entry_title) > 0) ? entry_title : url;
                } else if (namespace == 'M' || namespace == 'W' || namespace == 'X') {
                    /* Skip metadata, well-known, and index entries entirely */
                    continue;
                } else {
                    /* Resource entry (images, CSS, JS, fonts) — prefix with \x01 */
                    if (namespace == 'C') {
                        /* New-style namespace: store by path */
                        snprintf(hw_buf, sizeof(hw_buf), "\x01%s", url);
                    } else {
                        snprintf(hw_buf, sizeof(hw_buf), "\x01%c/%s", namespace, url);
                    }
                    headword = hw_buf;
                }
                
                uint64_t hw_off = 0;
                dict_cache_builder_add_headword(builder, headword, strlen(headword), &hw_off);
                temp_refs[valid_refs].h_off = hw_off;
                temp_refs[valid_refs].h_len = strlen(headword);
                temp_refs[valid_refs].cluster_idx = cluster_idx;
                temp_refs[valid_refs].blob_idx = blob_idx;
                valid_refs++;
            }
        }

        uint64_t *bin_cache_offsets = g_malloc0(sizeof(uint64_t) * valid_refs);
        uint32_t *bin_cache_lens = g_malloc0(sizeof(uint32_t) * valid_refs);

        GList **cluster_to_refs = g_malloc0(sizeof(GList*) * hdr.cluster_count);
        for (uint32_t i = 0; i < valid_refs; i++) {
            if (temp_refs[i].cluster_idx < hdr.cluster_count) {
                cluster_to_refs[temp_refs[i].cluster_idx] = g_list_prepend(cluster_to_refs[temp_refs[i].cluster_idx], GUINT_TO_POINTER(i));
            }
        }

        for (uint32_t i = 0; i < hdr.cluster_count; i++) {
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                g_free(cluster_to_refs); g_free(bin_cache_offsets); g_free(bin_cache_lens); g_free(temp_refs); g_free(redir_refs);
                dict_cache_builder_free(builder); unlink(cache_path); munmap(map, st_file.st_size); close(fd); g_free(title); g_strfreev(mime_types); return NULL;
            }
            if (i % 10 == 0) settings_scan_progress_notify(path, 20 + (int)(i * 70 / hdr.cluster_count));
            if (!cluster_to_refs[i]) continue;
            
            uint64_t c_start = read_u64le(p + hdr.cluster_ptr_pos + i * 8);
            uint64_t c_end = (i + 1 < hdr.cluster_count) ? read_u64le(p + hdr.cluster_ptr_pos + (i + 1) * 8) : hdr.checksum_pos;
            
            if (c_start >= (uint64_t)st_file.st_size || c_end > (uint64_t)st_file.st_size || c_start >= c_end) {
                g_list_free(cluster_to_refs[i]); continue;
            }
            
            const unsigned char *c_p = p + c_start;
            uint8_t raw_comp = *c_p;
            uint8_t comp_type = raw_comp & 0x0f;
            gboolean extended = (raw_comp & 0x10) != 0; /* ZIM v6: 8-byte blob offsets */
            int offset_size = extended ? 8 : 4;
            size_t comp_size = c_end - c_start - 1;
            
            size_t decomp_len = 0;
            unsigned char *decomp = zim_decompress(comp_type, c_p + 1, comp_size, &decomp_len);
            if (decomp) {
                uint64_t first_offset = extended ? read_u64le(decomp) : read_u32le(decomp);
                uint32_t blob_count = (uint32_t)(first_offset / offset_size);
                if (blob_count > 0) blob_count--; // Last offset is end of data
                
                for (GList *l = cluster_to_refs[i]; l; l = l->next) {
                    uint32_t ref_idx = GPOINTER_TO_UINT(l->data);
                    uint32_t b_idx = temp_refs[ref_idx].blob_idx;
                    if (b_idx < blob_count) {
                        uint64_t b_off = extended ? read_u64le(decomp + b_idx * offset_size) : read_u32le(decomp + b_idx * offset_size);
                        uint64_t b_end_off = extended ? read_u64le(decomp + (b_idx + 1) * offset_size) : read_u32le(decomp + (b_idx + 1) * offset_size);
                        if (b_end_off >= b_off && b_end_off <= decomp_len) {
                            uint32_t b_len = (uint32_t)(b_end_off - b_off);
                            uint64_t def_off = 0;
                            dict_cache_builder_add_definition(builder, (const char *)decomp + b_off, b_len, &def_off);
                            bin_cache_offsets[ref_idx] = def_off;
                            bin_cache_lens[ref_idx] = b_len;
                        }
                    }
                }
                g_free(decomp);
            }
            g_list_free(cluster_to_refs[i]);
        }
        g_free(cluster_to_refs);

        TreeEntry *final_entries = g_malloc_n(valid_refs + redir_count, sizeof(TreeEntry));
        size_t final_cnt = 0;
        for (uint32_t i = 0; i < valid_refs; i++) {
            if (bin_cache_lens[i] > 0) {
                final_entries[final_cnt].h_off = temp_refs[i].h_off;
                final_entries[final_cnt].h_len = temp_refs[i].h_len;
                final_entries[final_cnt].d_off = bin_cache_offsets[i];
                final_entries[final_cnt].d_len = bin_cache_lens[i];
                final_cnt++;
            }
        }

        /* Resolve redirects: for each redirect, find the target entry's definition.
         * We do a simple lookup: scan the URL pointer list for the target index to
         * get its cluster/blob, then find if we already cached that data. */
        for (uint32_t r = 0; r < redir_count; r++) {
            uint32_t target = redir_refs[r].redirect_target;
            if (target >= hdr.article_count) continue;
            /* Look up the target's dir entry to get cluster/blob */
            uint64_t tgt_dir_off = read_u64le(p + hdr.url_ptr_pos + (uint64_t)target * 8);
            if (tgt_dir_off + 16 > (uint64_t)st_file.st_size) continue;
            const unsigned char *tgt_dp = p + tgt_dir_off;
            uint16_t tgt_mime = read_u16le(tgt_dp);
            if (tgt_mime == 0xffff || tgt_mime == 0xfffe || tgt_mime == 0xfffd) continue;
            uint32_t tgt_cluster = read_u32le(tgt_dp + 8);
            uint32_t tgt_blob = read_u32le(tgt_dp + 12);
            /* Search our temp_refs for a matching cluster/blob */
            for (uint32_t i = 0; i < valid_refs; i++) {
                if (temp_refs[i].cluster_idx == tgt_cluster && temp_refs[i].blob_idx == tgt_blob && bin_cache_lens[i] > 0) {
                    final_entries[final_cnt].h_off = redir_refs[r].h_off;
                    final_entries[final_cnt].h_len = redir_refs[r].h_len;
                    final_entries[final_cnt].d_off = bin_cache_offsets[i];
                    final_entries[final_cnt].d_len = bin_cache_lens[i];
                    final_cnt++;
                    break;
                }
            }
        }

        g_free(temp_refs); g_free(bin_cache_offsets); g_free(bin_cache_lens); g_free(redir_refs);

        dict_cache_builder_flush(builder);
        dict_cache_builder_finalize(builder, final_entries, final_cnt);

        if (final_cnt > 0) {
            char *hw_path = dict_hw_index_path_for(path);
            int hw_fd = open(cache_path, O_RDONLY);
            if (hw_fd >= 0) {
                struct stat hw_st;
                if (fstat(hw_fd, &hw_st) == 0 && hw_st.st_size > 0) {
                    const char *hw_map = mmap(NULL, (size_t)hw_st.st_size, PROT_READ, MAP_PRIVATE, hw_fd, 0);
                    if (hw_map != MAP_FAILED) {
                        DictHwBuilder *hw = dict_hw_builder_new(hw_path);
                        if (hw) {
                            for (size_t i = 0; i < final_cnt; i++) {
                                dict_hw_builder_add(hw, hw_map + final_entries[i].h_off, final_entries[i].h_len, final_entries[i].d_off, final_entries[i].d_len);
                            }
                            dict_hw_builder_set_metadata(hw, "source_path", path);
                            if (title && *title) {
                                dict_hw_builder_set_metadata(hw, "dict_name", title);
                            }
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
    g_strfreev(mime_types);

    char *hw_path = dict_hw_index_path_for(path);
    int cache_fd = open(cache_path, O_RDONLY);
    if (cache_fd < 0) { g_free(hw_path); g_free(cache_path); g_free(title); return NULL; }
    struct stat st_cache;
    if (fstat(cache_fd, &st_cache) < 0) { close(cache_fd); g_free(hw_path); g_free(cache_path); g_free(title); return NULL; }
    void *cache_map = mmap(NULL, st_cache.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (cache_map == MAP_FAILED) { close(cache_fd); g_free(hw_path); g_free(cache_path); g_free(title); return NULL; }

    DictMmap *dm = g_new0(DictMmap, 1);
    dm->fd = cache_fd;
    close(dm->fd); dm->fd = -1;
    dm->data = (const char*)cache_map;
    dm->size = st_cache.st_size;
    dm->name = title;
    dm->source_dir = g_path_get_dirname(path);
    dm->index = flat_index_open(hw_path);
    g_free(hw_path);
    if (dict_cache_is_compressed(dm->data, dm->size)) {
        dm->is_compressed = TRUE;
        dm->chunk_reader = dict_chunk_reader_new(dm->data, dm->size, (const DictCacheHeader*)dm->data);
    }

    char *res_dir = g_build_filename(g_get_user_cache_dir(), "diction", "zim_res", dm->name, NULL);
    /* Set resource_dir so the renderer can resolve <img>, <link>, etc. paths */
    dm->resource_dir = g_strdup(res_dir);
    ZimResourceBackend *backend = g_new0(ZimResourceBackend, 1);
    backend->dm = dm;
    dm->resource_reader = resource_reader_new(res_dir, backend, zim_res_get, zim_res_has, zim_res_close);
    g_free(res_dir);

    settings_scan_progress_notify(path, 100);
    g_free(cache_path);
    return dm;
}
