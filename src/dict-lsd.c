#include "dict-mmap.h"
#include "dict-cache.h"
#include "dict-cache-builder.h"
#include "flat-index.h"
#include "settings.h"
#
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#
/* Native LSD (.lsd) support (ABBYY Lingvo).
 *
 * Phase 1 implementation: supports Lingvo user dictionaries matching the same
 * decoding path as lsd2dsl's UserDictionaryDecoder(false), notably version 0x142001.
 *
 * This parser builds a Diction cache (DictCacheBuilder + FlatIndex) directly,
 * without converting to DSL files.
 */
#

#pragma pack(push, 1)
typedef struct {
    char magic[8];                  /* "LingVo\0\0\0" typically */
    uint32_t version;               /* little-endian on disk */
    uint32_t unk;
    uint32_t checksum;
    uint32_t entriesCount;
    uint32_t annotationOffset;
    uint32_t dictionaryEncoderOffset;
    uint32_t articlesOffset;
    uint32_t pagesOffset;
    uint32_t unk1;
    uint16_t lastPage;
    uint16_t unk3;
    uint16_t sourceLanguage;
    uint16_t targetLanguage;
} LSDHeader;
#pragma pack(pop)

typedef struct {
    FILE *f;
    uint8_t bit_pos; /* 0..7 */
    uint8_t cache;
} LsdBitStream;

static gboolean lsd_seek(LsdBitStream *bs, uint32_t pos) {
    if (!bs || !bs->f) return FALSE;
    if (fseek(bs->f, (long)pos, SEEK_SET) != 0) return FALSE;
    bs->bit_pos = 0;
    bs->cache = 0;
    return TRUE;
}

static uint32_t lsd_tell(LsdBitStream *bs) {
    if (!bs || !bs->f) return 0;
    long p = ftell(bs->f);
    if (p < 0) return 0;
    return (uint32_t)p;
}

static void lsd_to_nearest_byte(LsdBitStream *bs) {
    if (!bs) return;
    bs->bit_pos = 0;
}

static size_t lsd_read_some(LsdBitStream *bs, void *dst, size_t n) {
    if (!bs || !bs->f || !dst || n == 0) return 0;
    /* lsd2dsl's readSome ignores bit alignment; in practice callers ensure byte alignment.
       For safety we force byte alignment here. */
    lsd_to_nearest_byte(bs);
    return fread(dst, 1, n, bs->f);
}

static guint lsd_read_bit(LsdBitStream *bs) {
    if (bs->bit_pos == 0) {
        if (fread(&bs->cache, 1, 1, bs->f) != 1) return 0;
    }
    /* MSB-first: bit 7..0 */
    guint bit = (bs->cache >> (7 - bs->bit_pos)) & 1U;
    bs->bit_pos = (uint8_t)((bs->bit_pos + 1) & 7);
    return bit;
}

static guint lsd_read_bits(LsdBitStream *bs, guint n) {
    guint res = 0;
    for (guint i = 0; i < n; i++) {
        res = (res << 1) | lsd_read_bit(bs);
    }
    return res;
}

static guint BitLength(guint num) {
    guint res = 1;
    while ((num >>= 1) != 0) res++;
    return res;
}

static uint16_t reverse16(uint16_t n) {
    return (uint16_t)((n >> 8) | (n << 8));
}

static uint32_t reverse32(uint32_t n) {
    return ((n & 0x000000FFU) << 24) |
           ((n & 0x0000FF00U) << 8)  |
           ((n & 0x00FF0000U) >> 8)  |
           ((n & 0xFF000000U) >> 24);
}

static DictMmap* open_cache_dict(const char *cache_path, const char *source_path) {
    int cache_fd = open(cache_path, O_RDONLY);
    if (cache_fd < 0) return NULL;

    struct stat st;
    if (fstat(cache_fd, &st) != 0 || st.st_size <= 0) {
        close(cache_fd);
        return NULL;
    }

    const char *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (data == MAP_FAILED) {
        close(cache_fd);
        return NULL;
    }

    DictMmap *dm = g_new0(DictMmap, 1);
    dm->fd = cache_fd;
    dm->data = data;
    dm->size = (size_t)st.st_size;
    dm->tmp_file = NULL;
    dm->source_dir = source_path ? g_path_get_dirname(source_path) : NULL;
    dm->name = source_path ? g_path_get_basename(source_path) : g_strdup("LSD Dictionary");
    dm->index = flat_index_open(dm->data, dm->size);
    if (dict_cache_is_compressed(dm->data, dm->size)) {
        dm->is_compressed = TRUE;
        dm->chunk_reader = dict_chunk_reader_new(dm->data, dm->size, (const DictCacheHeader*)dm->data);
    }
    if (!dm->index) {
        munmap((void*)dm->data, dm->size);
        close(dm->fd);
        g_free(dm->source_dir);
        g_free(dm->name);
        if (dm->chunk_reader) dict_chunk_reader_free(dm->chunk_reader);
        g_free(dm);
        return NULL;
    }
    return dm;
}

static gunichar2* read_unicode_string(LsdBitStream *bs, int len, gboolean big_endian, int *out_units) {
    if (out_units) *out_units = 0;
    if (len <= 0) return NULL;
    gunichar2 *buf = g_new(gunichar2, (gsize)len + 1);
    for (int i = 0; i < len; i++) {
        uint16_t ch = 0;
        if (lsd_read_some(bs, &ch, 2) != 2) { g_free(buf); return NULL; }
        if (big_endian) ch = reverse16(ch);
        buf[i] = (gunichar2)ch;
    }
    buf[len] = 0;
    if (out_units) *out_units = len;
    return buf;
}

static guint32* read_symbols(LsdBitStream *bs, int *out_len) {
    if (out_len) *out_len = 0;
    int len = (int)lsd_read_bits(bs, 32);
    int bits_per_symbol = (int)lsd_read_bits(bs, 8);
    if (len <= 0 || bits_per_symbol <= 0 || bits_per_symbol > 32) return NULL;
    guint32 *syms = g_new(guint32, (gsize)len);
    for (int i = 0; i < len; i++) {
        syms[i] = (guint32)lsd_read_bits(bs, (guint)bits_per_symbol);
    }
    if (out_len) *out_len = len;
    return syms;
}

static gboolean read_reference(LsdBitStream *bs, guint *out_ref, guint huffman_number) {
    if (!out_ref) return FALSE;
    guint code = lsd_read_bits(bs, 2);
    if (code == 3) {
        *out_ref = lsd_read_bits(bs, 32);
        return TRUE;
    }
    guint bitlen = BitLength(huffman_number);
    if (bitlen < 2) bitlen = 2;
    *out_ref = (code << (bitlen - 2)) | lsd_read_bits(bs, bitlen - 2);
    return TRUE;
}

typedef struct {
    int left;   /* 0 empty; >0 child index+1; <0 leaf (-1-symIdx) */
    int right;
    int parent;
    int weight;
} HuffmanNode;

typedef struct {
    HuffmanNode *nodes;
    guint32 nodes_len;
    int *symidx2nodeidx;
    guint32 symidx_count;
    int nextNodePosition;
} LenTable;

static gboolean lentable_place_symidx(LenTable *lt, int symIdx, int nodeIdx, int len) {
    if (!lt || !lt->nodes || len <= 0) return FALSE;

    if (len == 1) {
        if (lt->nodes[nodeIdx].left == 0) {
            lt->nodes[nodeIdx].left = -1 - symIdx;
            lt->symidx2nodeidx[symIdx] = nodeIdx;
            return TRUE;
        }
        if (lt->nodes[nodeIdx].right == 0) {
            lt->nodes[nodeIdx].right = -1 - symIdx;
            lt->symidx2nodeidx[symIdx] = nodeIdx;
            return TRUE;
        }
        return FALSE;
    }

    if (lt->nodes[nodeIdx].left == 0) {
        if (lt->nextNodePosition >= (int)lt->nodes_len - 1) return FALSE;
        lt->nodes[lt->nextNodePosition] = (HuffmanNode){0, 0, nodeIdx, -1};
        lt->nodes[nodeIdx].left = ++lt->nextNodePosition; /* store as 1-based */
    }
    if (lt->nodes[nodeIdx].left > 0) {
        if (lentable_place_symidx(lt, symIdx, lt->nodes[nodeIdx].left - 1, len - 1))
            return TRUE;
    }
    if (lt->nodes[nodeIdx].right == 0) {
        if (lt->nextNodePosition >= (int)lt->nodes_len - 1) return FALSE;
        lt->nodes[lt->nextNodePosition] = (HuffmanNode){0, 0, nodeIdx, -1};
        lt->nodes[nodeIdx].right = ++lt->nextNodePosition;
    }
    if (lt->nodes[nodeIdx].right > 0) {
        if (lentable_place_symidx(lt, symIdx, lt->nodes[nodeIdx].right - 1, len - 1))
            return TRUE;
    }
    return FALSE;
}

static void lentable_free(LenTable *lt) {
    if (!lt) return;
    g_free(lt->nodes);
    g_free(lt->symidx2nodeidx);
    memset(lt, 0, sizeof(*lt));
}

static gboolean lentable_read(LenTable *lt, LsdBitStream *bs) {
    if (!lt || !bs) return FALSE;
    lentable_free(lt);
    int count = (int)lsd_read_bits(bs, 32);
    int bitsPerLen = (int)lsd_read_bits(bs, 8);
    if (count <= 1 || bitsPerLen <= 0 || bitsPerLen > 32) return FALSE;
    int idxBitSize = (int)BitLength((guint)count);

    lt->symidx_count = (guint32)count;
    lt->symidx2nodeidx = g_new(int, (gsize)count);
    for (int i = 0; i < count; i++) lt->symidx2nodeidx[i] = -1;

    lt->nodes_len = (guint32)(count - 1);
    lt->nodes = g_new0(HuffmanNode, (gsize)lt->nodes_len);
    int rootIdx = (int)lt->nodes_len - 1;
    lt->nodes[rootIdx] = (HuffmanNode){0, 0, -1, -1};
    lt->nextNodePosition = 0;

    for (int i = 0; i < count; i++) {
        int symidx = (int)lsd_read_bits(bs, (guint)idxBitSize);
        int len = (int)lsd_read_bits(bs, (guint)bitsPerLen);
        if (symidx < 0 || symidx >= count) return FALSE;
        if (!lentable_place_symidx(lt, symidx, rootIdx, len)) return FALSE;
    }
    return TRUE;
}

static int lentable_decode(const LenTable *lt, LsdBitStream *bs, guint *out_symIdx) {
    if (!lt || !lt->nodes || lt->nodes_len == 0 || !bs || !out_symIdx) return 0;
    const HuffmanNode *node = &lt->nodes[lt->nodes_len - 1]; /* root */
    int len = 0;
    for (;;) {
        len++;
        int bit = (int)lsd_read_bits(bs, 1);
        if (bit) {
            if (node->right < 0) { *out_symIdx = (guint)(-1 - node->right); return len; }
            node = &lt->nodes[node->right - 1];
        } else {
            if (node->left < 0) { *out_symIdx = (guint)(-1 - node->left); return len; }
            node = &lt->nodes[node->left - 1];
        }
    }
}

typedef struct {
    gunichar2 *prefix;
    int prefix_units;
    guint32 *articleSymbols;
    int articleSymbols_len;
    guint32 *headingSymbols;
    int headingSymbols_len;
    LenTable ltArticles;
    LenTable ltHeadings;
    LenTable ltPrefixLengths;
    LenTable ltPostfixLengths;
    guint huffman1Number;
    guint huffman2Number;
} UserDecoder;

static void userdecoder_free(UserDecoder *d) {
    if (!d) return;
    g_free(d->prefix);
    g_free(d->articleSymbols);
    g_free(d->headingSymbols);
    lentable_free(&d->ltArticles);
    lentable_free(&d->ltHeadings);
    lentable_free(&d->ltPrefixLengths);
    lentable_free(&d->ltPostfixLengths);
    memset(d, 0, sizeof(*d));
}

static gboolean userdecoder_read(UserDecoder *d, LsdBitStream *bs) {
    if (!d || !bs) return FALSE;

    int len = (int)lsd_read_bits(bs, 32);
    d->prefix = read_unicode_string(bs, len, TRUE, &d->prefix_units);
    if (!d->prefix && len > 0) return FALSE;

    d->articleSymbols = read_symbols(bs, &d->articleSymbols_len);
    d->headingSymbols = read_symbols(bs, &d->headingSymbols_len);
    if (!d->articleSymbols || !d->headingSymbols) return FALSE;

    if (!lentable_read(&d->ltArticles, bs)) return FALSE;
    if (!lentable_read(&d->ltHeadings, bs)) return FALSE;
    if (!lentable_read(&d->ltPrefixLengths, bs)) return FALSE;
    if (!lentable_read(&d->ltPostfixLengths, bs)) return FALSE;

    d->huffman1Number = lsd_read_bits(bs, 32);
    d->huffman2Number = lsd_read_bits(bs, 32);
    return TRUE;
}

static gboolean userdecoder_decode_prefix_len(const UserDecoder *d, LsdBitStream *bs, guint *out_len) {
    return lentable_decode(&d->ltPrefixLengths, bs, out_len) > 0;
}

static gboolean userdecoder_decode_postfix_len(const UserDecoder *d, LsdBitStream *bs, guint *out_len) {
    return lentable_decode(&d->ltPostfixLengths, bs, out_len) > 0;
}

static gboolean userdecoder_decode_heading(const UserDecoder *d, LsdBitStream *bs, guint len, gunichar2 **out_u16, int *out_units) {
    if (!out_u16 || !out_units) return FALSE;
    *out_u16 = NULL;
    *out_units = 0;
    gunichar2 *buf = g_new(gunichar2, (gsize)len + 1);
    for (guint i = 0; i < len; i++) {
        guint symIdx = 0;
        lentable_decode(&d->ltHeadings, bs, &symIdx);
        if ((int)symIdx < 0 || (int)symIdx >= d->headingSymbols_len) { g_free(buf); return FALSE; }
        guint32 sym = d->headingSymbols[symIdx];
        if (sym > 0xFFFFU) { g_free(buf); return FALSE; }
        buf[i] = (gunichar2)sym;
    }
    buf[len] = 0;
    *out_u16 = buf;
    *out_units = (int)len;
    return TRUE;
}

static gboolean userdecoder_decode_article(const UserDecoder *d, LsdBitStream *bs, gunichar2 **out_u16, int *out_units) {
    if (!out_u16 || !out_units) return FALSE;
    *out_u16 = NULL;
    *out_units = 0;

    guint maxlen = lsd_read_bits(bs, 16);
    if (maxlen == 0xFFFFU) {
        maxlen = lsd_read_bits(bs, 32);
    }
    if (maxlen == 0) return FALSE;

    GArray *arr = g_array_sized_new(FALSE, FALSE, sizeof(gunichar2), maxlen + 8);
    while ((guint)arr->len < maxlen) {
        guint symIdx = 0;
        lentable_decode(&d->ltArticles, bs, &symIdx);
        if ((int)symIdx < 0 || (int)symIdx >= d->articleSymbols_len) { g_array_free(arr, TRUE); return FALSE; }

        guint32 sym = d->articleSymbols[symIdx];
        if (sym >= 0x10000U) {
            if (sym >= 0x10040U) {
                guint startIdx = lsd_read_bits(bs, BitLength(maxlen));
                guint copy_len = sym - 0x1003dU;
                if (startIdx + copy_len > (guint)arr->len) { g_array_free(arr, TRUE); return FALSE; }
                /* Copy from already-built output: must snapshot because append may reallocate. */
                gunichar2 *tmp = g_memdup2(((gunichar2*)arr->data) + startIdx, copy_len * sizeof(gunichar2));
                g_array_append_vals(arr, tmp, copy_len);
                g_free(tmp);
            } else {
                guint startIdx = lsd_read_bits(bs, BitLength((guint)d->prefix_units));
                guint copy_len = sym - 0xfffdU;
                if (startIdx + copy_len > (guint)d->prefix_units) { g_array_free(arr, TRUE); return FALSE; }
                g_array_append_vals(arr, d->prefix + startIdx, copy_len);
            }
        } else {
            gunichar2 ch = (gunichar2)(sym & 0xFFFFU);
            g_array_append_val(arr, ch);
        }
    }

    gunichar2 nul = 0;
    g_array_append_val(arr, nul);
    *out_units = (int)arr->len - 1;
    *out_u16 = (gunichar2*)g_array_free(arr, FALSE);
    return TRUE;
}

typedef struct {
    gboolean isLeaf;
    guint headingsCount;
} CachePageHdr;

static gboolean cachepage_load_header(LsdBitStream *bs, CachePageHdr *out) {
    if (!bs || !out) return FALSE;
    memset(out, 0, sizeof(*out));
    out->isLeaf = lsd_read_bits(bs, 1) ? TRUE : FALSE;
    (void)lsd_read_bits(bs, 16); /* number */
    (void)lsd_read_bits(bs, 16); /* prev */
    (void)lsd_read_bits(bs, 16); /* parent */
    (void)lsd_read_bits(bs, 16); /* next */
    out->headingsCount = lsd_read_bits(bs, 16);
    lsd_to_nearest_byte(bs);
    return TRUE;
}

static char* u16_to_utf8(const gunichar2 *u16, int units) {
    if (!u16 || units <= 0) return g_strdup("");
    GError *err = NULL;
    char *utf8 = g_utf16_to_utf8(u16, units, NULL, NULL, &err);
    if (err) {
        g_error_free(err);
        return g_strdup("");
    }
    return utf8 ? utf8 : g_strdup("");
}

DictMmap* parse_lsd_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (!path) return NULL;
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    LSDHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return NULL;
    }

    if (memcmp(hdr.magic, "LingVo", 6) != 0) {
        fclose(f);
        return NULL;
    }

    /* Phase 1: only implement 0x142001 (UserDictionaryDecoder(false)) */
    if (hdr.version != 0x142001U) {
        fprintf(stderr, "[LSD] Unsupported LSD version 0x%06x for %s\n", hdr.version, path);
        fclose(f);
        return NULL;
    }

    dict_cache_ensure_dir();
    char *cache_path = dict_cache_path_for(path);
    gboolean cache_valid = dict_cache_is_valid(cache_path, path);

    if (cache_valid) {
        DictMmap *dm = open_cache_dict(cache_path, path);
        if (dm) {
            g_free(cache_path);
            fclose(f);
            return dm;
        }
        cache_valid = FALSE;
    }

    if (!dict_cache_prepare_target_path(cache_path, 0)) {
        g_free(cache_path);
        fclose(f);
        return NULL;
    }

    LsdBitStream bs = { .f = f, .bit_pos = 0, .cache = 0 };
    /* We already read the header via fread; reset and re-read via bitstream for consistency. */
    if (!lsd_seek(&bs, 0)) { g_free(cache_path); fclose(f); return NULL; }
    if (lsd_read_some(&bs, &hdr, sizeof(hdr)) != sizeof(hdr)) { g_free(cache_path); fclose(f); return NULL; }

    /* Create and load decoder from dictionaryEncoderOffset */
    UserDecoder dec = {0};
    if (!lsd_seek(&bs, hdr.dictionaryEncoderOffset)) {
        g_free(cache_path);
        fclose(f);
        return NULL;
    }
    if (!userdecoder_read(&dec, &bs)) {
        userdecoder_free(&dec);
        g_free(cache_path);
        fclose(f);
        return NULL;
    }

    /* Walk pages and collect entries. */
    guint pages_count = (guint)hdr.lastPage + 1U;
    guint64 estimated = hdr.entriesCount > 0 ? hdr.entriesCount : 0;
    DictCacheBuilder *builder = dict_cache_builder_new(cache_path, estimated);
    if (!builder) {
        userdecoder_free(&dec);
        g_free(cache_path);
        fclose(f);
        return NULL;
    }

    size_t entry_cap = (size_t)(estimated > 0 ? estimated : 8192);
    if (entry_cap < 1024) entry_cap = 1024;
    FlatTreeEntry *entries = g_new0(FlatTreeEntry, entry_cap);
    size_t entry_count = 0;

    for (guint page = 0; page < pages_count; page++) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        if (!lsd_seek(&bs, hdr.pagesOffset + 512U * page)) break;

        CachePageHdr ph;
        if (!cachepage_load_header(&bs, &ph)) break;
        if (!ph.isLeaf) continue;

        /* knownPrefix as UTF-16 */
        GArray *known = g_array_new(FALSE, FALSE, sizeof(gunichar2));
        for (guint idx = 0; idx < ph.headingsCount; idx++) {
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;

            guint prefixLen = 0, postfixLen = 0;
            if (!userdecoder_decode_prefix_len(&dec, &bs, &prefixLen)) break;
            if (!userdecoder_decode_postfix_len(&dec, &bs, &postfixLen)) break;

            gunichar2 *postfix = NULL;
            int postfix_units = 0;
            if (!userdecoder_decode_heading(&dec, &bs, postfixLen, &postfix, &postfix_units)) break;

            guint reference = 0;
            if (!read_reference(&bs, &reference, dec.huffman2Number)) { g_free(postfix); break; }

            /* Optional ext pairs; ignore content but advance stream exactly like lsd2dsl */
            guint has_pairs = lsd_read_bits(&bs, 1);
            if (has_pairs) {
                guint pair_len = lsd_read_bits(&bs, 8);
                for (guint p = 0; p < pair_len; p++) {
                    (void)lsd_read_bits(&bs, 8);  /* idx */
                    (void)lsd_read_bits(&bs, 16); /* chr */
                }
            }

            /* known = known[0:prefixLen] + postfix */
            if (prefixLen < (guint)known->len) {
                g_array_set_size(known, prefixLen);
            } else if (prefixLen > (guint)known->len) {
                /* If stream says prefixLen longer than we have, pad with nothing. */
            }
            if (postfix_units > 0) {
                g_array_append_vals(known, postfix, postfix_units);
            }
            g_free(postfix);

            /* Decode article */
            uint32_t saved_pos = lsd_tell(&bs);
            uint8_t saved_bit = bs.bit_pos;
            uint8_t saved_cache = bs.cache;
            if (!lsd_seek(&bs, hdr.articlesOffset + reference)) break;
            gunichar2 *article_u16 = NULL;
            int article_units = 0;
            if (!userdecoder_decode_article(&dec, &bs, &article_u16, &article_units)) { g_free(article_u16); break; }
            /* Restore page stream position (may be mid-byte) */
            if (fseek(bs.f, (long)saved_pos, SEEK_SET) != 0) { g_free(article_u16); break; }
            bs.bit_pos = saved_bit;
            bs.cache = saved_cache;

            /* Convert both key and value to UTF-8 */
            char *hw_utf8 = u16_to_utf8((const gunichar2*)known->data, (int)known->len);
            char *def_utf8 = u16_to_utf8(article_u16, article_units);
            g_free(article_u16);

            if (hw_utf8 && *hw_utf8 && def_utf8) {
                if (entry_count >= entry_cap) {
                    entry_cap *= 2;
                    entries = g_renew(FlatTreeEntry, entries, entry_cap);
                }

                uint64_t hw_off = 0, def_off = 0;
                size_t hw_len = strlen(hw_utf8);
                size_t def_len = strlen(def_utf8);

                dict_cache_builder_add_headword(builder, hw_utf8, hw_len, &hw_off);
                dict_cache_builder_add_definition(builder, def_utf8, def_len, &def_off);

                entries[entry_count].h_off = hw_off;
                entries[entry_count].h_len = (uint32_t)hw_len;
                entries[entry_count].d_off = def_off;
                entries[entry_count].d_len = (uint32_t)def_len;
                entry_count++;
            }

            g_free(hw_utf8);
            g_free(def_utf8);
        }
        g_array_free(known, TRUE);
    }

    /* Finalize cache */
    dict_cache_builder_flush(builder);

    /* Sort using a mapping of the newly written cache file */
    int tmp_fd = open(cache_path, O_RDONLY);
    if (tmp_fd >= 0) {
        struct stat st;
        if (fstat(tmp_fd, &st) == 0 && st.st_size > 0) {
            void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, tmp_fd, 0);
            if (map != MAP_FAILED) {
                flat_index_sort_entries(entries, entry_count, (const char*)map, (size_t)st.st_size);
                munmap(map, (size_t)st.st_size);
            }
        }
        close(tmp_fd);
    }

    dict_cache_builder_finalize(builder, entries, (uint64_t)entry_count);
    dict_cache_builder_free(builder);
    g_free(entries);

    /* Sync cache mtime to source */
    const char *sources[] = { path };
    dict_cache_sync_mtime(cache_path, sources, 1);

    userdecoder_free(&dec);
    fclose(f);

    DictMmap *dm = open_cache_dict(cache_path, path);
    g_free(cache_path);
    return dm;
}

