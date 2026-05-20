#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <bzlib.h>
#include <glib.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "dict-cache.h"
#include "dict-cache-builder.h"
#include "dict-chunked.h"
#include "settings.h"

#pragma pack(push, 1)
typedef struct {
    char signature[4];
    char inputLang[3];
    char outputLang[3];
    uint8_t compression;
    uint32_t wordCount;
    uint32_t shortIndexLength;
    uint32_t titleOffset;
    uint32_t copyrightOffset;
    uint32_t versionOffset;
    uint32_t shortIndexOffset;
    uint32_t fullIndexOffset;
    uint32_t articlesOffset;
} DCTHeader;

typedef struct {
    uint16_t nextWord;
    uint16_t previousWord;
    uint32_t articleOffset;
} IndexElement;
#pragma pack(pop)

static char* decompress_zlib(const unsigned char *src, size_t src_len, size_t *out_len) {
    if (!src || src_len == 0) return NULL;
    z_stream strm = {0};
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = (uInt)src_len;
    strm.next_in = (Bytef *)src;

    if (inflateInit(&strm) != Z_OK) return NULL;

    size_t cap = src_len * 4 + 1024;
    unsigned char *dest = g_malloc(cap + 1);
    strm.avail_out = (uInt)cap;
    strm.next_out = dest;

    int ret;
    while ((ret = inflate(&strm, Z_NO_FLUSH)) != Z_STREAM_END) {
        if (ret == Z_BUF_ERROR || ret == Z_OK) {
            size_t used = cap - strm.avail_out;
            cap *= 2;
            dest = g_realloc(dest, cap + 1);
            strm.next_out = dest + used;
            strm.avail_out = (uInt)(cap - used);
        } else {
            inflateEnd(&strm);
            g_free(dest);
            return NULL;
        }
    }

    *out_len = (size_t)strm.total_out;
    inflateEnd(&strm);
    if (*out_len > cap) *out_len = cap; /* safety */
    dest[*out_len] = '\0';
    return (char*)dest;
}

static char* decompress_bz2(const unsigned char *src, size_t src_len, size_t *out_len) {
    if (!src || src_len == 0) return NULL;
    bz_stream strm = {0};
    strm.bzalloc = NULL;
    strm.bzfree = NULL;
    strm.opaque = NULL;
    strm.avail_in = (unsigned int)src_len;
    strm.next_in = (char *)src;

    if (BZ2_bzDecompressInit(&strm, 0, 0) != BZ_OK) return NULL;

    size_t cap = src_len * 4 + 1024;
    unsigned char *dest = g_malloc(cap + 1);
    strm.avail_out = (unsigned int)cap;
    strm.next_out = (char *)dest;

    int ret;
    while ((ret = BZ2_bzDecompress(&strm)) != BZ_STREAM_END) {
        if (ret == BZ_OK) {
            size_t used = cap - strm.avail_out;
            cap *= 2;
            dest = g_realloc(dest, cap + 1);
            strm.next_out = (char *)(dest + used);
            strm.avail_out = (unsigned int)(cap - used);
        } else {
            BZ2_bzDecompressEnd(&strm);
            g_free(dest);
            return NULL;
        }
    }

    *out_len = cap - strm.avail_out;
    BZ2_bzDecompressEnd(&strm);
    dest[*out_len] = '\0';
    return (char*)dest;
}

static char* convert_sdict_markup(const char *in, size_t in_len) {
    if (!in || in_len == 0) return g_strdup("");
    GString *out = g_string_new("");
    gboolean after_eol = FALSE;

    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0) continue; /* Skip embedded NULLs which break string logic */
        
        if (c == '\n') {
            after_eol = TRUE;
            g_string_append(out, "<br/>");
        } else if (c == ' ' && after_eol) {
            g_string_append(out, "&nbsp;");
        } else {
            g_string_append_c(out, (char)c);
            after_eol = FALSE;
        }
    }

    char *s = g_string_free(out, FALSE);
    
    static GRegex *sdict_regexes[8] = {NULL};
    static GRegex *sdict_link_reg = NULL;
    static gsize sdict_regex_init = 0;

    if (g_once_init_enter(&sdict_regex_init)) {
        const char *patterns[] = {
            "<\\s*(p|br)\\s*>",
            "<\\s*/p\\s*>",
            "<\\s*t\\s*>",
            "<\\s*f\\s*>",
            "<\\s*/t\\s*>",
            "<\\s*/f\\s*>",
            "<\\s*l\\s*>",
            "<\\s*/l\\s*>"
        };
        for (int i = 0; i < 8; i++) {
            sdict_regexes[i] = g_regex_new(patterns[i], G_REGEX_CASELESS | G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
        }
        sdict_link_reg = g_regex_new("<\\s*r\\s*>(.*?)<\\s*/r\\s*>", G_REGEX_CASELESS | G_REGEX_DOTALL | G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
        g_once_init_leave(&sdict_regex_init, 1);
    }

    const char *replacements[] = {
        "<br/>",
        "",
        "<span class=\"sdict_tr\" dir=\"ltr\">[",
        "<span class=\"sdict_forms\">",
        "]</span>",
        "</span>",
        "<ul>",
        "</ul>"
    };

    for (int i = 0; i < 8; i++) {
        if (sdict_regexes[i]) {
            char *next = g_regex_replace_literal(sdict_regexes[i], s, -1, 0, replacements[i], 0, NULL);
            if (next) {
                g_free(s);
                s = next;
            }
        }
    }

    if (sdict_link_reg) {
        char *next = g_regex_replace(sdict_link_reg, s, -1, 0, "<a class=\"sdict_wordref\" href=\"bword://\\1\">\\1</a>", 0, NULL);
        if (next) {
            g_free(s);
            s = next;
        }
    }

    /* Final safety: ensure valid UTF-8 for the UI engine */
    char *valid_utf8 = g_utf8_make_valid(s, -1);
    g_free(s);
    return valid_utf8;
}

DictMmap* parse_sdict_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    fprintf(stderr, "[SDICT] Loading: %s\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    DCTHeader header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return NULL;
    }

    if (strncmp(header.signature, "sdct", 4) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t compression = header.compression & 0x0F;
    
    /* Load title */
    fseek(f, header.titleOffset, SEEK_SET);
    uint32_t title_size = 0;
    fread(&title_size, 4, 1, f);
    unsigned char *title_raw = g_malloc(title_size);
    fread(title_raw, 1, title_size, f);

    char *bookname = NULL;
    size_t out_len = 0;
    if (compression == 1) bookname = decompress_zlib(title_raw, title_size, &out_len);
    else if (compression == 2) bookname = decompress_bz2(title_raw, title_size, &out_len);
    else bookname = g_strndup((char*)title_raw, title_size);
    g_free(title_raw);

    dict_cache_ensure_dir();
    char *cache_path = dict_cache_path_for(path);
    const char *sources[] = {path};

    if (dict_cache_is_valid(cache_path, path)) {
        int fd = open(cache_path, O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0) {
                const char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (data != MAP_FAILED) {
                    fprintf(stderr, "[SDICT] Cache loaded successfully: %s\n", cache_path);
                    DictMmap *dict = g_new0(DictMmap, 1);
                    dict->fd = fd;
                    close(dict->fd);
                    dict->fd = -1;
                    dict->data = data;
                    dict->size = st.st_size;
                    dict->name = bookname;
                    dict->index = flat_index_open(dict->data, dict->size);
                    dict->is_compressed = TRUE;
                    dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, (const DictCacheHeader*)dict->data);
                    g_free(cache_path);
                    fclose(f);
                    return dict;
                } else {
                    fprintf(stderr, "[SDICT] Cache mmap failed: %s (size=%zu)\n", cache_path, (size_t)st.st_size);
                }
            }
            close(fd);
        }
    }

    /* Build cache */
    fprintf(stderr, "[SDICT] Building cache: %s (words=%u)\n", cache_path, header.wordCount);
    fflush(stderr);
    DictCacheBuilder *builder = dict_cache_builder_new(cache_path, header.wordCount);
    fprintf(stderr, "[SDICT] Cache builder: %p\n", (void*)builder);
    fflush(stderr);
    if (!builder) {
        fprintf(stderr, "[SDICT] Failed to create cache builder for %s\n", cache_path);
        g_free(bookname);
        g_free(cache_path);
        fclose(f);
        return NULL;
    }

    if (header.wordCount > 1000000) {
         fprintf(stderr, "[SDICT] WordCount too large: %u\n", header.wordCount);
         dict_cache_builder_free(builder);
         g_free(bookname);
         g_free(cache_path);
         fclose(f);
         return NULL;
    }
    
    struct stat st_dict;
    uint64_t dict_file_size = 0;
    if (fstat(fileno(f), &st_dict) == 0) dict_file_size = st_dict.st_size;
    fprintf(stderr, "[SDICT] dict_file_size=%lu\n", (unsigned long)dict_file_size);
    fflush(stderr);

    FlatTreeEntry *entries = g_malloc0(header.wordCount * sizeof(FlatTreeEntry));
    fprintf(stderr, "[SDICT] entries alloc OK: %p\n", (void*)entries);
    fflush(stderr);
    uint32_t count = 0;
    uint32_t pos = header.fullIndexOffset;

    for (uint32_t i = 0; i < header.wordCount; i++) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        
        if (pos + 8 > dict_file_size) {
            fprintf(stderr, "[SDICT] Index overflow at entry %u, pos %u\n", i, pos);
            break;
        }
        
        if (fseek(f, pos, SEEK_SET) != 0) {
            fprintf(stderr, "[SDICT] Seek failed at pos %u\n", pos);
            break;
        }
        IndexElement el;
        if (fread(&el, 1, sizeof(el), f) != sizeof(el)) {
            fprintf(stderr, "[SDICT] Read failed at pos %u\n", pos);
            break;
        }
        
        if (el.nextWord < sizeof(el)) {
            fprintf(stderr, "[SDICT] Invalid nextWord %u at pos %u\n", el.nextWord, pos);
            break;
        }
        uint32_t word_size = el.nextWord - sizeof(el);
        if (word_size > 4096) {
             /* Headword too long, skip this entry or limit it */
             pos += el.nextWord;
             continue;
        }

        char *word_buf = g_malloc0(word_size + 1);
        if (word_size > 0) {
            fread(word_buf, 1, word_size, f);
        }

        /* Load article */
        uint64_t art_file_pos = (uint64_t)header.articlesOffset + el.articleOffset;
        if (art_file_pos + 4 <= dict_file_size && fseek(f, (long)art_file_pos, SEEK_SET) == 0) {
            uint32_t art_size = 0;
            if (fread(&art_size, 4, 1, f) == 1) {
                if (art_size > 0 && art_size < 10*1024*1024) {
                    if (art_file_pos + 4 + art_size > dict_file_size) {
                         fprintf(stderr, "[SDICT] Article overflow at entry %u: pos=%lu size=%u file_size=%lu\n", i, (unsigned long)art_file_pos, art_size, (unsigned long)dict_file_size);
                    } else {
                        unsigned char *art_raw = g_malloc0(art_size + 1);
                        if (fread(art_raw, 1, art_size, f) == art_size) {
                    char *art_text = NULL;
                    size_t art_text_len = 0;
                    if (compression == 1) art_text = decompress_zlib(art_raw, art_size, &art_text_len);
                    else if (compression == 2) art_text = decompress_bz2(art_raw, art_size, &art_text_len);
                    else {
                        art_text = g_strndup((char*)art_raw, art_size);
                        art_text_len = art_size;
                    }

                    if (art_text) {
                        char *html = convert_sdict_markup(art_text, art_text_len);
                        if (html) {
                            uint64_t hw_off = 0, def_off = 0;
                            size_t html_len = strlen(html);
                            dict_cache_builder_add_headword(builder, word_buf, word_size, &hw_off);
                            dict_cache_builder_add_definition(builder, html, html_len, &def_off);
                            
                            entries[count].h_off = hw_off;
                            entries[count].h_len = word_size;
                            entries[count].d_off = def_off;
                            entries[count].d_len = strlen(html);
                            count++;
                            g_free(html);
                        }
                        g_free(art_text);
                    }
                }
                        g_free(art_raw);
                    }
                }
            }
        }
        g_free(word_buf);
        pos += el.nextWord;

        if ((i + 1) % 1000 == 0 && header.wordCount > 0) {
            settings_scan_progress_notify(path, (int)((i + 1) * 100 / header.wordCount));
            fprintf(stderr, "[SDICT] Indexed %u/%u entries...\n", i + 1, header.wordCount);
        }
    }

    fprintf(stderr, "[SDICT] Loop finished. count=%u\n", count);
    dict_cache_builder_flush(builder);

    /* Sort entries using the written cache data */
    if (count > 0) {
        int sort_fd = open(cache_path, O_RDONLY);
        if (sort_fd >= 0) {
            struct stat sort_st;
            if (fstat(sort_fd, &sort_st) == 0 && sort_st.st_size > 0) {
                void *sort_data = mmap(NULL, sort_st.st_size, PROT_READ, MAP_PRIVATE, sort_fd, 0);
                if (sort_data != MAP_FAILED) {
                    flat_index_sort_entries(entries, count, sort_data, sort_st.st_size);
                    munmap(sort_data, sort_st.st_size);
                }
            }
            close(sort_fd);
        }
    }

    dict_cache_builder_finalize(builder, entries, count);
    dict_cache_builder_free(builder);
    dict_cache_sync_mtime(cache_path, sources, 1);

    g_free(entries);
    g_free(cache_path);
    fclose(f);

    /* Open the newly built cache */
    char *final_cache_path = dict_cache_path_for(path);
    int fd = open(final_cache_path, O_RDONLY);
    g_free(final_cache_path);

    if (fd < 0) {
        g_free(bookname);
        return NULL;
    }
    struct stat final_st;
    if (fstat(fd, &final_st) != 0) {
        close(fd);
        g_free(bookname);
        return NULL;
    }
    const char *final_data = mmap(NULL, final_st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (final_data == MAP_FAILED) {
        close(fd);
        g_free(bookname);
        return NULL;
    }
    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = fd;
    close(dict->fd);
    dict->fd = -1;
    dict->data = final_data;
    dict->size = final_st.st_size;
    dict->name = bookname;
    dict->index = flat_index_open(dict->data, dict->size);
    if (dict_cache_is_compressed(dict->data, dict->size)) {
        dict->is_compressed = TRUE;
        dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, (const DictCacheHeader*)dict->data);
    }
    return dict;
}

