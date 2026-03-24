/* dict-mdx.c — MDict (.mdx) dictionary parser
 *
 * Faithfully follows goldendict-ng/src/dict/mdictparser.cc layout.
 */

#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <utime.h>

/* Cache helpers for persistent dictionary storage */
static const char* get_cache_base_dir(void) {
    static const char *cache_dir = NULL;
    if (!cache_dir) cache_dir = g_get_user_cache_dir();
    return cache_dir;
}

static char* get_cache_dir_path(void) {
    const char *base = get_cache_base_dir();
    return g_build_filename(base, "diction", "dicts", NULL);
}

static char* get_cached_dict_path(const char *original_path) {
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, original_path, -1);
    const char *base = get_cache_base_dir();
    char *path = g_build_filename(base, "diction", "dicts", hash, NULL);
    g_free(hash);
    return path;
}

static gboolean is_cache_valid(const char *cache_path, const char *original_path) {
    struct stat cache_st, orig_st;
    if (stat(cache_path, &cache_st) != 0 || stat(original_path, &orig_st) != 0)
        return FALSE;
    return cache_st.st_mtime >= orig_st.st_mtime;
}

static gboolean ensure_cache_directory(void) {
    char *cache_dir = get_cache_dir_path();
    int ret = g_mkdir_with_parents(cache_dir, 0755);
    g_free(cache_dir);
    return ret == 0;
}

/* ── endian helpers ──────────────────────────────────── */

static uint32_t ru32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t ru64be(const unsigned char *p) {
    return ((uint64_t)ru32be(p) << 32) | ru32be(p + 4);
}

static uint64_t read_num(const unsigned char **pp, int num_size) {
    uint64_t v;
    if (num_size == 8) { v = ru64be(*pp); *pp += 8; }
    else               { v = ru32be(*pp); *pp += 4; }
    return v;
}

static uint32_t read_u8or16(const unsigned char **pp, int is_v2) {
    if (is_v2) { uint32_t v = ((*pp)[0] << 8) | (*pp)[1]; *pp += 2; return v; }
    else       { uint32_t v = (*pp)[0]; *pp += 1; return v; }
}

/* ── zlib decompression ─────────────────────────────── */

static unsigned char *zlib_inflate(const unsigned char *src, size_t src_len,
                                    size_t hint, size_t *out_len) {
    size_t cap = hint ? hint : src_len * 4;
    unsigned char *dst = malloc(cap);
    if (!dst) return NULL;

    z_stream zs = {0};
    zs.next_in  = (unsigned char *)src;
    zs.avail_in = src_len;
    zs.next_out = dst;
    zs.avail_out = cap;

    if (inflateInit(&zs) != Z_OK) { free(dst); return NULL; }
    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);

    if (ret != Z_STREAM_END && ret != Z_OK) { free(dst); return NULL; }
    *out_len = zs.total_out;
    return dst;
}

static unsigned char *mdx_block_decompress(const unsigned char *block,
                                            size_t comp_size,
                                            size_t decomp_hint,
                                            size_t *out_len) {
    if (comp_size <= 8) return NULL;
    uint32_t type = ru32be(block);
    const unsigned char *payload = block + 8;
    size_t payload_len = comp_size - 8;

    if (type == 0x00000000) {
        unsigned char *c = malloc(payload_len);
        if (!c) return NULL;
        memcpy(c, payload, payload_len);
        *out_len = payload_len;
        return c;
    }
    if (type == 0x02000000) {
        return zlib_inflate(payload, payload_len, decomp_hint, out_len);
    }
    return NULL;
}

#include "ripemd128.h"

static void mdx_decrypt_key_block_info(unsigned char *buf, size_t len) {
    if (len <= 8) return;
    RIPEMD128_CTX ctx;
    ripemd128_init(&ctx);
    ripemd128_update(&ctx, buf + 4, 4);
    ripemd128_update(&ctx, (const uint8_t *)"\x95\x36\x00\x00", 4);
    uint8_t key[16];
    ripemd128_digest(&ctx, key);
    unsigned char *p = buf + 8;
    size_t dlen = len - 8;
    uint8_t prev = 0x36;
    for (size_t i = 0; i < dlen; i++) {
        uint8_t byte = p[i];
        byte = (byte >> 4) | (byte << 4);
        byte = byte ^ prev ^ (uint8_t)(i & 0xFF) ^ key[i % 16];
        prev = p[i];
        p[i] = byte;
    }
}

static size_t utf16le_to_utf8(const unsigned char *src, size_t src_bytes,
                                char *dst, size_t dst_cap) {
    size_t si = 0, di = 0;
    while (si + 1 < src_bytes && di + 4 < dst_cap) {
        uint32_t ch = src[si] | ((uint32_t)src[si+1] << 8);
        si += 2;
        if (ch >= 0xD800 && ch <= 0xDBFF && si + 1 < src_bytes) {
            uint32_t lo = src[si] | ((uint32_t)src[si+1] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                si += 2;
                ch = 0x10000 + ((ch & 0x3FF) << 10) + (lo & 0x3FF);
            }
        }
        if (ch < 0x80)       { dst[di++] = ch; }
        else if (ch < 0x800) { dst[di++] = 0xC0|(ch>>6); dst[di++] = 0x80|(ch&0x3F); }
        else if (ch < 0x10000) { dst[di++] = 0xE0|(ch>>12); dst[di++] = 0x80|((ch>>6)&0x3F); dst[di++] = 0x80|(ch&0x3F); }
        else { dst[di++] = 0xF0|(ch>>18); dst[di++] = 0x80|((ch>>12)&0x3F); dst[di++] = 0x80|((ch>>6)&0x3F); dst[di++] = 0x80|(ch&0x3F); }
    }
    dst[di] = '\0';
    return di;
}

static void mdx_normalize_resource_path(char *p) {
    char *d = p;
    while (*p == '\\' || *p == '/' || *p == '.') p++;
    while (*p) {
        if (*p == '\\') *d++ = '/';
        else *d++ = *p;
        p++;
    }
    *d = '\0';
}

typedef struct { uint64_t off; char *name; } MDDRes;
static int mdd_res_cmp(const void *a, const void *b) {
    uint64_t oa = ((MDDRes*)a)->off, ob = ((MDDRes*)b)->off;
    return (oa < ob) ? -1 : (oa > ob) ? 1 : 0;
}

static void mdx_extract_mdd_resources(const char *mdd_path, const char *dest_dir, int is_v2, int num_size, int encoding_is_utf16, int encrypted) {
    FILE *f = fopen(mdd_path, "rb");
    if (!f) return;
    unsigned char b4[4];
    if (fread(b4, 1, 4, f) != 4) { fclose(f); return; }
    uint32_t hts = ru32be(b4);
    fseek(f, hts + 4, SEEK_CUR);
    int kbh_size = is_v2 ? (num_size * 5) : (num_size * 4);
    unsigned char *kbh = malloc(kbh_size);
    if (!kbh) { fclose(f); return; }
    if (fread(kbh, 1, kbh_size, f) != (size_t)kbh_size) { free(kbh); fclose(f); return; }
    const unsigned char *kp = kbh;
    uint64_t num_key_blocks = read_num(&kp, num_size);
    uint64_t num_entries = read_num(&kp, num_size);
    uint64_t kbi_decomp = is_v2 ? read_num(&kp, num_size) : 0;
    uint64_t kbi_comp = read_num(&kp, num_size);
    uint64_t kb_data_size = read_num(&kp, num_size);
    free(kbh);
    if (is_v2) fseek(f, 4, SEEK_CUR);
    unsigned char *kbi_raw = malloc(kbi_comp);
    if (!kbi_raw) { fclose(f); return; }
    if (fread(kbi_raw, 1, kbi_comp, f) != kbi_comp) { free(kbi_raw); fclose(f); return; }
    long kb_data_pos = ftell(f);
    typedef struct { uint64_t comp, decomp; } KBI;
    KBI *kbis = calloc(num_key_blocks, sizeof(KBI));
    if (!kbis) { free(kbi_raw); fclose(f); return; }
    size_t kbc = 0;
    if (is_v2) {
        if (encrypted & 2) mdx_decrypt_key_block_info(kbi_raw, kbi_comp);
        size_t dlen = 0;
        unsigned char *data = mdx_block_decompress(kbi_raw, kbi_comp, kbi_decomp, &dlen);
        if (data) {
            const unsigned char *ip = data, *ie = data + dlen;
            while (ip < ie && kbc < num_key_blocks) {
                ip += num_size;
                uint32_t head_size = read_u8or16(&ip, 1); ip += (encoding_is_utf16 ? (head_size+1)*2 : (head_size+1));
                uint32_t tail_size = read_u8or16(&ip, 1); ip += (encoding_is_utf16 ? (tail_size+1)*2 : (tail_size+1));
                if (ip + num_size * 2 > ie) break;
                kbis[kbc].comp = read_num(&ip, num_size);
                kbis[kbc].decomp = read_num(&ip, num_size);
                kbc++;
            }
            free(data);
        }
    } else {
        const unsigned char *ip = kbi_raw, *ie = kbi_raw + kbi_comp;
        while (ip < ie && kbc < num_key_blocks) {
            ip += num_size;
            uint32_t head_size = read_u8or16(&ip, 0); ip += (head_size+1);
            uint32_t tail_size = read_u8or16(&ip, 0); ip += (tail_size+1);
            if (ip + num_size * 2 > ie) break;
            kbis[kbc].comp = read_num(&ip, num_size);
            kbis[kbc].decomp = read_num(&ip, num_size);
            kbc++;
        }
    }
    free(kbi_raw);
    MDDRes *resources = calloc(num_entries, sizeof(MDDRes));
    if (!resources) { free(kbis); fclose(f); return; }
    size_t res_count = 0;
    fseek(f, kb_data_pos, SEEK_SET);
    for (size_t bi = 0; bi < kbc; bi++) {
        unsigned char *comp = malloc(kbis[bi].comp);
        if (!comp) continue;
        if (fread(comp, 1, kbis[bi].comp, f) != kbis[bi].comp) { free(comp); continue; }
        size_t dlen = 0;
        unsigned char *data = mdx_block_decompress(comp, kbis[bi].comp, kbis[bi].decomp, &dlen);
        free(comp);
        if (!data) continue;
        const unsigned char *hp = data, *he = data + dlen;
        while (hp < he && res_count < num_entries) {
            if (hp + num_size > he) break;
            resources[res_count].off = (num_size == 8) ? ru64be(hp) : ru32be(hp);
            hp += num_size;
            char word[1024];
            if (encoding_is_utf16) {
                const unsigned char *ws = hp;
                while (hp + 1 < he && !(hp[0] == 0 && hp[1] == 0)) hp += 2;
                utf16le_to_utf8(ws, hp - ws, word, sizeof(word)-1);
                if (hp + 1 < he) hp += 2;
            } else {
                const unsigned char *ws = hp;
                while (hp < he && *hp != '\0') hp++;
                size_t wl = hp - ws; if (wl > 1023) wl = 1023;
                memcpy(word, ws, wl); word[wl] = '\0';
                if (hp < he) hp++;
            }
            mdx_normalize_resource_path(word);
            resources[res_count].name = strdup(word);
            res_count++;
        }
        free(data);
    }
    qsort(resources, res_count, sizeof(MDDRes), mdd_res_cmp);
    fseek(f, kb_data_pos + kb_data_size, SEEK_SET);
    unsigned char rbh[64]; if (fread(rbh, 1, num_size * 4, f) != (size_t)(num_size * 4)) {}
    const unsigned char *rp = rbh;
    uint64_t nrb = read_num(&rp, num_size);
    read_num(&rp, num_size);
    read_num(&rp, num_size);
    typedef struct { uint64_t comp, decomp; } RBI;
    RBI *rbis = calloc(nrb, sizeof(RBI));
    if (rbis) {
        for (uint64_t i = 0; i < nrb; i++) {
            unsigned char p[16]; if(fread(p, 1, num_size * 2, f) != (size_t)(num_size * 2)) break;
            const unsigned char *pp = p;
            rbis[i].comp = read_num(&pp, num_size);
            rbis[i].decomp = read_num(&pp, num_size);
        }
        uint64_t td = 0;
        for (uint64_t i = 0; i < nrb; i++) td += rbis[i].decomp;
        unsigned char *all_recs = malloc(td);
        if (all_recs) {
            uint64_t co = 0;
            for (uint64_t i = 0; i < nrb; i++) {
                unsigned char *comp = malloc(rbis[i].comp);
                if (!comp) continue;
                if (fread(comp, 1, rbis[i].comp, f) == rbis[i].comp) {
                    size_t dlen = 0;
                    unsigned char *data = mdx_block_decompress(comp, rbis[i].comp, rbis[i].decomp, &dlen);
                    if (data) { memcpy(all_recs + co, data, dlen); co += dlen; free(data); }
                }
                free(comp);
            }
            for (size_t i = 0; i < res_count; i++) {
                if (!resources[i].name[0]) continue;
                uint64_t start = resources[i].off;
                uint64_t end = (i + 1 < res_count) ? resources[i+1].off : td;
                if (start < td && end <= td && end > start) {
                    char *full = g_build_filename(dest_dir, resources[i].name, NULL);
                    char *parent = g_path_get_dirname(full);
                    g_mkdir_with_parents(parent, 0755);
                    FILE *rf = fopen(full, "wb");
                    if (rf) { fwrite(all_recs + start, 1, (size_t)(end - start), rf); fclose(rf); }
                    g_free(full); g_free(parent);
                }
            }
            free(all_recs);
        }
        free(rbis);
    }
    for(size_t i=0; i<res_count; i++) free(resources[i].name);
    free(resources); free(kbis); fclose(f);
}

typedef struct { long h_off; size_t h_len; long d_off; size_t d_len; } TreeEntry;

static void insert_balanced(SplayTree *t, TreeEntry *e, int start, int end) {
    if (start > end) return;
    int mid = start + (end - start) / 2;
    if (e[mid].h_len > 0)
        splay_tree_insert(t, e[mid].h_off, e[mid].h_len, e[mid].d_off, e[mid].d_len);
    insert_balanced(t, e, start, mid - 1);
    insert_balanced(t, e, mid + 1, end);
}

DictMmap* parse_mdx_file(const char *path) {
    // Ensure cache directory exists
    ensure_cache_directory();

    // Get cache path for this dictionary
    char *cache_path = get_cached_dict_path(path);
    gboolean cache_exists = (access(cache_path, F_OK) == 0);
    gboolean cache_valid = cache_exists && is_cache_valid(cache_path, path);

    int cache_fd = -1;
    const char *dict_data = NULL;
    size_t dict_size = 0;

    if (cache_valid) {
        // Use cached version directly
        printf("[MDX] Loading from cache: %s\n", cache_path);
        cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd < 0) {
            g_free(cache_path);
            return NULL;
        }

        struct stat st;
        if (fstat(cache_fd, &st) < 0 || st.st_size == 0) {
            close(cache_fd);
            g_free(cache_path);
            return NULL;
        }
        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
        if (dict_data == MAP_FAILED) {
            close(cache_fd);
            g_free(cache_path);
            return NULL;
        }

        // Read MDX header for resource extraction metadata (only if .mdd exists)
        FILE *fh = NULL;
        char *mdd_check = g_strdup(path);
        size_t mpl = strlen(mdd_check);
        if (mpl > 4) strcpy(mdd_check + mpl - 3, "mdd");
        if (g_file_test(mdd_check, G_FILE_TEST_EXISTS)) {
            fh = fopen(path, "rb");
        }
        g_free(mdd_check);

        int is_v2 = 0, num_size = 4, encoding_is_utf16 = 0, encrypted = 0;
        if (fh) {
            unsigned char buf4[4];
            if (fread(buf4, 1, 4, fh) == 4) {
                uint32_t header_text_size = ru32be(buf4);
                if (header_text_size <= 10*1024*1024) {
                    unsigned char *header_raw = malloc(header_text_size);
                    if (header_raw) {
                        fread(header_raw, 1, header_text_size, fh);
                        size_t ascii_len = header_text_size / 2;
                        char *ascii_hdr = malloc(ascii_len + 1);
                        if (ascii_hdr) {
                            for (size_t i = 0; i < ascii_len; i++) ascii_hdr[i] = header_raw[i * 2];
                            ascii_hdr[ascii_len] = '\0';
                            char *vp = strstr(ascii_hdr, "GeneratedByEngineVersion=\"");
                            if (vp) { double ver = atof(vp + 24); is_v2 = (ver >= 2.0); num_size = is_v2 ? 8 : 4; }
                            char *ep = strstr(ascii_hdr, "Encoding=\"");
                            if (ep && strncasecmp(ep + 10, "UTF-16", 6) == 0) encoding_is_utf16 = 1;
                            char *xp = strstr(ascii_hdr, "Encrypted=\"");
                            if (xp) encrypted = atoi(xp + 11);
                            free(ascii_hdr);
                        }
                        free(header_raw);
                    }
                }
            }
            fclose(fh);
        }

        // Build index from cached data
        DictMmap *dict = calloc(1, sizeof(DictMmap));
        dict->fd = cache_fd;
        dict->tmp_file = NULL;
        dict->data = dict_data;
        dict->size = dict_size;
        dict->index = splay_tree_new(dict->data, dict->size);

        // Parse cached file for indexing
        const char *dp = dict->data;
        const char *de = dp + dict->size;
        int indexed = 0;
        int max_entries = 1000000; // Safety limit

        while (dp < de && indexed < max_entries) {
            const char *hw_start = dp;
            while (dp < de && *dp != '\n') dp++;
            size_t hw_len = dp - hw_start;
            if (dp < de) dp++; // skip newline

            // Handle empty lines
            if (hw_len == 0 && dp >= de) break;
            if (hw_len == 0) continue;

            const char *def_start = dp;
            while (dp < de && *dp != '\n') dp++;
            size_t def_len = dp - def_start;
            if (dp < de) dp++; // skip newline

            if (def_len > 0) {
                splay_tree_insert(dict->index, hw_start - dict->data, hw_len, def_start - dict->data, def_len);
                indexed++;
            }
        }

        printf("[MDX] Loaded %d cached entries\n", indexed);

        // Defer resource extraction to background or skip on cache load
        // Resources will be extracted on first access if needed
        dict->resource_dir = NULL;

        g_free(cache_path);

        return dict;
    }

    // Need to parse and cache
    printf("[MDX] Building cache from source: %s\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) { g_free(cache_path); return NULL; }
    unsigned char buf4[4];
    if (fread(buf4, 1, 4, f) != 4) { fclose(f); return NULL; }
    uint32_t header_text_size = ru32be(buf4);
    if (header_text_size > 10*1024*1024) { fclose(f); return NULL; }
    unsigned char *header_raw = malloc(header_text_size);
    if (!header_raw) { fclose(f); return NULL; }
    fread(header_raw, 1, header_text_size, f);
    size_t ascii_len = header_text_size / 2;
    char *ascii_hdr = malloc(ascii_len + 1);
    if (ascii_hdr) {
        for (size_t i = 0; i < ascii_len; i++) ascii_hdr[i] = header_raw[i * 2];
        ascii_hdr[ascii_len] = '\0';
    }
    #define FIND_ATTR(name) ({ \
        const char *_key = name "=\""; \
        char *_p = ascii_hdr ? strstr(ascii_hdr, _key) : NULL; \
        _p ? (_p + strlen(_key)) : NULL; \
    })
    double version = 1.0;
    char *vp = FIND_ATTR("GeneratedByEngineVersion");
    if (vp) version = atof(vp);
    int encoding_is_utf16 = 0;
    char *ep = FIND_ATTR("Encoding");
    if (ep && strncasecmp(ep, "UTF-16", 6) == 0) encoding_is_utf16 = 1;
    int encrypted = 0;
    char *xp = FIND_ATTR("Encrypted");
    if (xp) encrypted = atoi(xp);
    #undef FIND_ATTR
    free(ascii_hdr); free(header_raw);
    fseek(f, 4, SEEK_CUR);
    int num_size = (version >= 2.0) ? 8 : 4;
    int is_v2 = (version >= 2.0);
    int kbh_size = is_v2 ? (num_size * 5) : (num_size * 4);
    unsigned char *kbh = malloc(kbh_size);
    if (!kbh) { fclose(f); return NULL; }
    fread(kbh, 1, kbh_size, f);
    const unsigned char *kp = kbh;
    uint64_t num_key_blocks = read_num(&kp, num_size);
    uint64_t num_entries = read_num(&kp, num_size);
    uint64_t kb_info_decomp_size = is_v2 ? read_num(&kp, num_size) : 0;
    uint64_t kb_info_comp_size = read_num(&kp, num_size);
    uint64_t kb_data_size = read_num(&kp, num_size);
    free(kbh);
    if (is_v2) fseek(f, 4, SEEK_CUR);
    long kb_info_file_pos = ftell(f);
    unsigned char *kb_info_raw = malloc(kb_info_comp_size);
    if (!kb_info_raw) { fclose(f); return NULL; }
    fread(kb_info_raw, 1, kb_info_comp_size, f);
    long kb_data_file_pos = ftell(f);
    typedef struct { uint64_t comp, decomp; } KBInfo;
    KBInfo *kb_infos = calloc(num_key_blocks + 1, sizeof(KBInfo));
    size_t kb_count = 0;
    if (is_v2) {
        if (encrypted & 2) mdx_decrypt_key_block_info(kb_info_raw, kb_info_comp_size);
        size_t dlen = 0;
        unsigned char *data = mdx_block_decompress(kb_info_raw, kb_info_comp_size, kb_info_decomp_size, &dlen);
        if (data) {
            const unsigned char *ip = data, *ie = data + dlen;
            while (ip < ie && kb_count < num_key_blocks) {
                ip += num_size;
                uint32_t hs = read_u8or16(&ip, 1); ip += (encoding_is_utf16 ? (hs+1)*2 : (hs+1));
                uint32_t ts = read_u8or16(&ip, 1); ip += (encoding_is_utf16 ? (ts+1)*2 : (ts+1));
                kb_infos[kb_count].comp = read_num(&ip, num_size);
                kb_infos[kb_count].decomp = read_num(&ip, num_size);
                kb_count++;
            }
            free(data);
        }
    } else {
        const unsigned char *ip = kb_info_raw, *ie = kb_info_raw + kb_info_comp_size;
        while (ip < ie && kb_count < num_key_blocks) {
            ip += num_size;
            uint32_t hs = read_u8or16(&ip, 0); ip += (hs+1);
            uint32_t ts = read_u8or16(&ip, 0); ip += (ts+1);
            kb_infos[kb_count].comp = read_num(&ip, num_size);
            kb_infos[kb_count].decomp = read_num(&ip, num_size);
            kb_count++;
        }
    }
    free(kb_info_raw);
    typedef struct { char *word; uint64_t offset; } HwEntry;
    HwEntry *hw_entries = calloc(num_entries + 1, sizeof(HwEntry));
    TreeEntry *tree_entries = NULL;
    size_t hw_count = 0;
    fseek(f, kb_data_file_pos, SEEK_SET);
    for (size_t bi = 0; bi < kb_count; bi++) {
        unsigned char *comp = malloc(kb_infos[bi].comp);
        if (!comp) continue;
        fread(comp, 1, kb_infos[bi].comp, f);
        size_t dlen = 0;
        unsigned char *data = mdx_block_decompress(comp, kb_infos[bi].comp, kb_infos[bi].decomp, &dlen);
        free(comp);
        if (!data) continue;
        const unsigned char *hp = data, *he = data + dlen;
        while (hp < he && hw_count < num_entries) {
            uint64_t id = (num_size == 8) ? ru64be(hp) : ru32be(hp);
            hp += num_size;
            char word[1024];
            if (encoding_is_utf16) {
                const unsigned char *ws = hp;
                while (hp + 1 < he && !(hp[0] == 0 && hp[1] == 0)) hp += 2;
                utf16le_to_utf8(ws, hp - ws, word, sizeof(word)-1);
                if (hp + 1 < he) hp += 2;
            } else {
                const unsigned char *ws = hp;
                while (hp < he && *hp != '\0') hp++;
                size_t wl = hp - ws; if (wl > 1023) wl = 1023;
                memcpy(word, ws, wl); word[wl] = '\0';
                if (hp < he) hp++;
            }
            hw_entries[hw_count].word = strdup(word);
            hw_entries[hw_count].offset = id;
            hw_count++;
        }
        free(data);
    }
    free(kb_infos);
    long rec_pos = kb_info_file_pos + (long)kb_info_comp_size + (long)kb_data_size;
    fseek(f, rec_pos, SEEK_SET);
    unsigned char rbh[64]; if (fread(rbh, 1, num_size * 4, f) != (size_t)(num_size * 4)) { /* handle err */ }
    const unsigned char *rp = rbh;
    uint64_t nrb = read_num(&rp, num_size);
    read_num(&rp, num_size);
    read_num(&rp, num_size);
    typedef struct { uint64_t comp, decomp; } RBInfo;
    RBInfo *rb_infos = calloc(nrb + 1, sizeof(RBInfo));
    for (uint64_t i = 0; i < nrb; i++) {
        unsigned char pair[16]; if(fread(pair, 1, num_size * 2, f) != (size_t)(num_size * 2)) break;
        const unsigned char *ppp = pair;
        rb_infos[i].comp = read_num(&ppp, num_size);
        rb_infos[i].decomp = read_num(&ppp, num_size);
    }

    // Write to cache file instead of tmpfile
    FILE *cache_file = fopen(cache_path, "wb");
    if (!cache_file) {
        fclose(f); free(rb_infos);
        for(size_t i=0; i<hw_count; i++) free(hw_entries[i].word);
        free(hw_entries);
        g_free(cache_path);
        return NULL;
    }

    uint64_t doff = 0;
    for (uint64_t i = 0; i < nrb; i++) {
        unsigned char *comp = malloc(rb_infos[i].comp);
        if (!comp) continue;
        fread(comp, 1, rb_infos[i].comp, f);
        size_t dlen = 0;
        unsigned char *data = mdx_block_decompress(comp, rb_infos[i].comp, rb_infos[i].decomp, &dlen);
        free(comp);
        if (data) { fwrite(data, 1, dlen, cache_file); doff += dlen; free(data); }
    }
    fclose(f); free(rb_infos);

    long cache_pos = ftell(cache_file);
    size_t def_data_size = cache_pos;
    void *def_data = malloc(def_data_size);
    if (!def_data) {
        fclose(cache_file);
        for(size_t i=0; i<hw_count; i++) free(hw_entries[i].word);
        free(hw_entries);
        g_free(cache_path);
        return NULL;
    }

    FILE *def_read = fopen(cache_path, "rb");
    fread(def_data, 1, def_data_size, def_read);
    fclose(def_read);

    tree_entries = calloc(hw_count + 1, sizeof(TreeEntry));
    for (size_t i = 0; i < hw_count; i++) {
        uint64_t d_start = hw_entries[i].offset;
        uint64_t d_end = (i + 1 < hw_count) ? hw_entries[i+1].offset : def_data_size;
        if (d_start < def_data_size && d_end <= def_data_size && d_end > d_start) {
            tree_entries[i].h_off = ftell(cache_file);
            tree_entries[i].h_len = strlen(hw_entries[i].word);
            fwrite(hw_entries[i].word, 1, tree_entries[i].h_len, cache_file);
            fwrite("\n", 1, 1, cache_file);
            tree_entries[i].d_off = ftell(cache_file);
            tree_entries[i].d_len = (size_t)(d_end - d_start);
            fwrite((unsigned char *)def_data + d_start, 1, tree_entries[i].d_len, cache_file);
            fwrite("\n", 1, 1, cache_file);
        }
    }
    free(def_data);
    fflush(cache_file);
    fclose(cache_file);

    // Update cache file mtime to match source
    struct stat src_st;
    if (stat(path, &src_st) == 0) {
        struct utimbuf times;
        times.actime = src_st.st_mtime;
        times.modtime = src_st.st_mtime;
        utime(cache_path, &times);
    }

    // Now open the cached file
    cache_fd = open(cache_path, O_RDONLY);
    if (cache_fd < 0) {
        for(size_t i=0; i<hw_count; i++) free(hw_entries[i].word);
        free(hw_entries); free(tree_entries);
        g_free(cache_path);
        return NULL;
    }

    struct stat st;
    if (fstat(cache_fd, &st) < 0 || st.st_size == 0) {
        close(cache_fd);
        for(size_t i=0; i<hw_count; i++) free(hw_entries[i].word);
        free(hw_entries); free(tree_entries);
        g_free(cache_path);
        return NULL;
    }

    dict_data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (dict_data == MAP_FAILED) {
        close(cache_fd);
        for(size_t i=0; i<hw_count; i++) free(hw_entries[i].word);
        free(hw_entries); free(tree_entries);
        g_free(cache_path);
        return NULL;
    }

    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = cache_fd;
    dict->tmp_file = NULL;
    dict->size = st.st_size;
    dict->data = dict_data;
    dict->index = splay_tree_new(dict->data, dict->size);
    insert_balanced(dict->index, tree_entries, 0, (int)hw_count - 1);
    free(tree_entries);
    printf("[MDX] Indexed %zu headwords (cached)\n", hw_count);

    // Extract resources
    char *mdd_path = g_strdup(path);
    size_t pl = strlen(mdd_path);
    if (pl > 4) strcpy(mdd_path + pl - 3, "mdd");
    if (g_file_test(mdd_path, G_FILE_TEST_EXISTS)) {
        char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
        char *root = g_build_filename(g_get_user_cache_dir(), "diction", "resources", hash, NULL);
        g_mkdir_with_parents(root, 0755);
        dict->resource_dir = g_strdup(root);
        mdx_extract_mdd_resources(mdd_path, root, is_v2, num_size, encoding_is_utf16, encrypted);
        g_free(hash); g_free(root);
    }
    g_free(mdd_path);
    g_free(cache_path);

    for (size_t i = 0; i < hw_count; i++) free(hw_entries[i].word);
    free(hw_entries);

    return dict;
}
