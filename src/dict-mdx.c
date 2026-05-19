/* dict-mdx.c — MDict (.mdx) dictionary parser
 *
 * Faithfully follows goldendict-ng/src/dict/mdictparser.cc layout.
 */

#include "dict-mmap.h"
#include "dict-cache-builder.h"
#include "langpair.h"
#include "text-utils.h"
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
#include <lzo/lzo1x.h>
#include <fcntl.h>
#include <utime.h>
#include <errno.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "settings.h"

/* Cache helpers for persistent dictionary storage */



/* Strip HTML tags from string */
static char* strip_html_tags(const char *input) {
    if (!input) return NULL;
    
    GString *result = g_string_new("");
    const char *p = input;
    
    while (*p) {
        if (*p == '<') {
            // Skip until closing >
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
        } else {
            g_string_append_c(result, *p);
            p++;
        }
    }
    
    return g_string_free(result, FALSE);
}

static char *unescape_xml_entities(const char *text) {
    if (!text) {
        return NULL;
    }

    GString *out = g_string_new("");
    const char *p = text;

    while (*p) {
        if (*p != '&') {
            g_string_append_c(out, *p++);
            continue;
        }

        const char *semi = strchr(p, ';');
        if (!semi) {
            g_string_append_c(out, *p++);
            continue;
        }

        size_t entity_len = semi - p + 1;
        if (g_str_has_prefix(p, "&lt;")) {
            g_string_append_c(out, '<');
        } else if (g_str_has_prefix(p, "&gt;")) {
            g_string_append_c(out, '>');
        } else if (g_str_has_prefix(p, "&amp;")) {
            g_string_append_c(out, '&');
        } else if (g_str_has_prefix(p, "&quot;")) {
            g_string_append_c(out, '"');
        } else if (g_str_has_prefix(p, "&apos;")) {
            g_string_append_c(out, '\'');
        } else if (entity_len >= 4 && p[1] == '#') {
            guint32 codepoint = 0;
            gboolean ok = FALSE;

            if ((p[2] == 'x' || p[2] == 'X') && entity_len > 4) {
                char *digits = g_strndup(p + 3, entity_len - 4);
                if (digits && *digits) {
                    char *endptr = NULL;
                    unsigned long parsed = strtoul(digits, &endptr, 16);
                    ok = endptr && *endptr == '\0' && g_unichar_validate((gunichar)parsed);
                    codepoint = (guint32)parsed;
                }
                g_free(digits);
            } else {
                char *digits = g_strndup(p + 2, entity_len - 3);
                if (digits && *digits) {
                    char *endptr = NULL;
                    unsigned long parsed = strtoul(digits, &endptr, 10);
                    ok = endptr && *endptr == '\0' && g_unichar_validate((gunichar)parsed);
                    codepoint = (guint32)parsed;
                }
                g_free(digits);
            }

            if (ok) {
                char utf8[7] = {0};
                int n = g_unichar_to_utf8((gunichar)codepoint, utf8);
                g_string_append_len(out, utf8, n);
            } else {
                g_string_append_len(out, p, entity_len);
            }
        } else {
            g_string_append_len(out, p, entity_len);
        }

        p = semi + 1;
    }

    return g_string_free(out, FALSE);
}

static char *extract_header_attribute(const char *header, const char *attr_name) {
    if (!header || !attr_name) {
        return NULL;
    }

    char *pattern = g_strdup_printf("%s=\"", attr_name);
    char *pos = strstr(header, pattern);
    g_free(pattern);
    if (!pos) {
        return NULL;
    }

    pos += strlen(attr_name) + 2;
    char *end = strchr(pos, '"');
    if (!end || end <= pos) {
        return NULL;
    }

    char *raw = g_strndup(pos, end - pos);
    char *unescaped = unescape_xml_entities(raw);
    g_free(raw);
    return unescaped;
}

#include "dict-cache.h"

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
    unsigned char *dst = g_malloc(cap);
    if (!dst) return NULL;

    z_stream zs = {0};
    zs.next_in  = (unsigned char *)src;
    zs.avail_in = src_len;
    zs.next_out = dst;
    zs.avail_out = cap;

    if (inflateInit(&zs) != Z_OK) { g_free(dst); return NULL; }
    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);

    if (ret != Z_STREAM_END && ret != Z_OK) { g_free(dst); return NULL; }
    *out_len = zs.total_out;
    return dst;
}

static unsigned char *lzo_inflate(const unsigned char *src, size_t src_len,
                                  size_t hint, size_t *out_len) {
    static gsize lzo_ready = 0;
    if (g_once_init_enter(&lzo_ready)) {
        gsize ok = (lzo_init() == LZO_E_OK) ? 1 : 0;
        g_once_init_leave(&lzo_ready, ok);
    }
    if (lzo_ready != 1) {
        return NULL;
    }

    size_t cap = hint ? hint : (src_len * 8 + 4096);
    if (cap == 0) {
        cap = 4096;
    }

    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned char *dst = g_malloc(cap);
        if (!dst) {
            return NULL;
        }

        lzo_uint actual = (lzo_uint)cap;
        int ret = lzo1x_decompress_safe(src, (lzo_uint)src_len, dst, &actual, NULL);
        if (ret == LZO_E_OK) {
            *out_len = (size_t)actual;
            return dst;
        }

        g_free(dst);
        if (ret != LZO_E_OUTPUT_OVERRUN) {
            return NULL;
        }
        cap *= 2;
    }

    return NULL;
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
        unsigned char *c = g_malloc(payload_len);
        if (!c) return NULL;
        memcpy(c, payload, payload_len);
        *out_len = payload_len;
        return c;
    }
    if (type == 0x01000000) {
        return lzo_inflate(payload, payload_len, decomp_hint, out_len);
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

static gboolean mdx_filename_matches_numbered_mdd(const char *filename,
                                                  const char *stem,
                                                  int *volume_out) {
    if (!filename || !stem || !g_str_has_suffix(filename, ".mdd")) {
        return FALSE;
    }

    gsize stem_len = strlen(stem);
    gsize filename_len = strlen(filename);
    if (filename_len <= stem_len + 4 || strncmp(filename, stem, stem_len) != 0) {
        return FALSE;
    }

    const char *suffix = filename + stem_len;
    const char *digits = suffix;
    const char *end = filename + filename_len - 4; /* before ".mdd" */

    if (*digits == '.') {
        digits++;
    }

    if (digits >= end) {
        return FALSE;
    }

    for (const char *p = digits; p < end; p++) {
        if (!g_ascii_isdigit(*p)) {
            return FALSE;
        }
    }

    if (volume_out) {
        *volume_out = atoi(digits);
    }
    return TRUE;
}

static int mdx_numbered_mdd_volume(const char *filename) {
    if (!filename || !g_str_has_suffix(filename, ".mdd")) {
        return 0;
    }

    gsize filename_len = strlen(filename);
    const char *digits_end = filename + filename_len - 4; /* before ".mdd" */
    const char *digits = digits_end;

    while (digits > filename && g_ascii_isdigit(*(digits - 1))) {
        digits--;
    }

    if (digits == digits_end) {
        return 0;
    }

    if (digits > filename && *(digits - 1) == '.') {
        return atoi(digits);
    }

    return atoi(digits);
}

static gint mdx_numbered_mdd_path_compare(gconstpointer a, gconstpointer b) {
    const char *path_a = *(char * const *)a;
    const char *path_b = *(char * const *)b;
    char *name_a = g_path_get_basename(path_a);
    char *name_b = g_path_get_basename(path_b);
    int vol_a = mdx_numbered_mdd_volume(name_a);
    int vol_b = mdx_numbered_mdd_volume(name_b);
    gint cmp = 0;

    if (vol_a != vol_b) {
        cmp = (vol_a < vol_b) ? -1 : 1;
    } else {
        cmp = g_strcmp0(name_a, name_b);
    }

    g_free(name_a);
    g_free(name_b);
    return cmp;
}

static GPtrArray *mdx_collect_mdd_paths(const char *mdx_path) {
    if (!mdx_path) {
        return NULL;
    }

    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    char *base = g_strdup(mdx_path);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
    }

    char *primary = g_strdup_printf("%s.mdd", base);
    if (g_file_test(primary, G_FILE_TEST_EXISTS)) {
        g_ptr_array_add(paths, primary);
    } else {
        g_free(primary);
    }

    char *dir_path = g_path_get_dirname(base);
    char *stem = g_path_get_basename(base);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (dir) {
        GPtrArray *numbered = g_ptr_array_new_with_free_func(g_free);
        const char *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            if (!mdx_filename_matches_numbered_mdd(name, stem, NULL)) {
                continue;
            }

            char *candidate = g_build_filename(dir_path, name, NULL);
            if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
                g_ptr_array_add(numbered, candidate);
            } else {
                g_free(candidate);
            }
        }
        g_dir_close(dir);

        g_ptr_array_sort(numbered, mdx_numbered_mdd_path_compare);
        for (guint i = 0; i < numbered->len; i++) {
            g_ptr_array_add(paths, g_ptr_array_index(numbered, i));
        }
        g_ptr_array_free(numbered, FALSE);
    }

    g_free(stem);
    g_free(dir_path);
    g_free(base);
    return paths;
}

typedef struct { 
    uint64_t comp; 
    uint64_t decomp; 
    uint64_t file_offset; 
    uint64_t decomp_offset; 
} MDD_RBI;

typedef struct {
    uint64_t comp_size;
    uint64_t decomp_size;
    char *head_word;
    char *tail_word;
    uint64_t file_offset;
} MDD_KBI;

typedef struct {
    char *mdd_path;
    int is_v2;
    int num_size;
    int encoding_is_utf16;
    int encrypted;
    long kb_data_pos;
    uint64_t kb_data_size;
    
    MDD_RBI *rbis;
    uint64_t nrb;
    uint64_t total_decomp;

    MDD_KBI *kbis;
    size_t nkb;
} MddFileState;

typedef struct {
    MddFileState **files;
    size_t num_files;
} MddBackend;

static gboolean mdd_find_resource(MddFileState *fs, const char *query, uint64_t *out_start_off, uint64_t *out_end_off) {
    if (!fs || !fs->kbis || fs->nkb == 0) return FALSE;
    
    int l = 0, r = (int)fs->nkb - 1;
    int found_kb = -1;
    while (l <= r) {
        int m = l + (r - l) / 2;
        int cmp_head = strcasecmp(query, fs->kbis[m].head_word);
        int cmp_tail = strcasecmp(query, fs->kbis[m].tail_word);
        
        if (cmp_head >= 0 && cmp_tail <= 0) {
            found_kb = m;
            break;
        } else if (cmp_head < 0) {
            r = m - 1;
        } else {
            l = m + 1;
        }
    }
    
    if (found_kb < 0) return FALSE;
    
    FILE *f = fopen(fs->mdd_path, "rb");
    if (!f) return FALSE;
    
    fseek(f, fs->kbis[found_kb].file_offset, SEEK_SET);
    unsigned char *comp = g_malloc(fs->kbis[found_kb].comp_size);
    if (!comp || fread(comp, 1, fs->kbis[found_kb].comp_size, f) != fs->kbis[found_kb].comp_size) {
        g_free(comp); fclose(f); return FALSE;
    }
    
    size_t dlen = 0;
    unsigned char *data = mdx_block_decompress(comp, fs->kbis[found_kb].comp_size, fs->kbis[found_kb].decomp_size, &dlen);
    g_free(comp);
    if (!data) { fclose(f); return FALSE; }
    
    gboolean found = FALSE;
    gboolean got_end = FALSE;
    const unsigned char *hp = data, *he = data + dlen;
    
    while (hp < he) {
        if (hp + fs->num_size > he) break;
        uint64_t current_off = (fs->num_size == 8) ? ru64be(hp) : ru32be(hp);
        hp += fs->num_size;
        
        char word[1024];
        if (fs->encoding_is_utf16) {
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
        
        if (found) {
            if (out_end_off) *out_end_off = current_off;
            got_end = TRUE;
            break;
        }
        
        if (strcasecmp(query, word) == 0) {
            found = TRUE;
            if (out_start_off) *out_start_off = current_off;
        }
    }
    g_free(data);
    
    if (found && !got_end) {
        if (found_kb + 1 < fs->nkb) {
            fseek(f, fs->kbis[found_kb + 1].file_offset, SEEK_SET);
            unsigned char *next_comp = g_malloc(fs->kbis[found_kb + 1].comp_size);
            if (next_comp && fread(next_comp, 1, fs->kbis[found_kb + 1].comp_size, f) == fs->kbis[found_kb + 1].comp_size) {
                size_t ndlen = 0;
                unsigned char *next_data = mdx_block_decompress(next_comp, fs->kbis[found_kb + 1].comp_size, fs->kbis[found_kb + 1].decomp_size, &ndlen);
                if (next_data && ndlen >= (size_t)fs->num_size) {
                    if (out_end_off) *out_end_off = (fs->num_size == 8) ? ru64be(next_data) : ru32be(next_data);
                }
                g_free(next_data);
            }
            g_free(next_comp);
        } else {
            if (out_end_off) *out_end_off = fs->total_decomp;
        }
    }
    
    fclose(f);
    return found;
}

static char* mdd_get(ResourceReader *reader, const char *name) {
    if (!reader || !name) return NULL;
    MddBackend *mdd = resource_reader_get_backend(reader);
    if (!mdd) return NULL;

    char *query = g_strdup(name);
    mdx_normalize_resource_path(query);

    for (size_t f = 0; f < mdd->num_files; f++) {
        MddFileState *fs = mdd->files[f];
        uint64_t start_off = 0, end_off = 0;
        
        if (mdd_find_resource(fs, query, &start_off, &end_off)) {
            char *dest_path = g_build_filename(resource_reader_get_dir(reader), query, NULL);
            if (g_file_test(dest_path, G_FILE_TEST_EXISTS)) {
                g_free(query);
                return dest_path;
            }

            char *parent_dir = g_path_get_dirname(dest_path);
            g_mkdir_with_parents(parent_dir, 0755);
            g_free(parent_dir);

            FILE *in = fopen(fs->mdd_path, "rb");
            if (!in) { g_free(dest_path); continue; }

            FILE *out = fopen(dest_path, "wb");
            if (!out) { fclose(in); g_free(dest_path); continue; }

            for (uint64_t i = 0; i < fs->nrb; i++) {
                uint64_t b_dstart = fs->rbis[i].decomp_offset;
                uint64_t b_dend = b_dstart + fs->rbis[i].decomp;
                
                if (end_off <= b_dstart || start_off >= b_dend) continue;
                
                fseek(in, fs->rbis[i].file_offset, SEEK_SET);
                unsigned char *comp = g_malloc(fs->rbis[i].comp);
                if (!comp || fread(comp, 1, fs->rbis[i].comp, in) != fs->rbis[i].comp) {
                    g_free(comp);
                    continue;
                }
                
                size_t dlen = 0;
                unsigned char *decomp = mdx_block_decompress(comp, fs->rbis[i].comp, fs->rbis[i].decomp, &dlen);
                g_free(comp);
                
                if (decomp) {
                    uint64_t slice_start = (start_off > b_dstart) ? start_off : b_dstart;
                    uint64_t slice_end = (end_off < b_dend) ? end_off : b_dend;
                    
                    size_t offset_in_block = slice_start - b_dstart;
                    size_t slice_len = slice_end - slice_start;
                    
                    fwrite(decomp + offset_in_block, 1, slice_len, out);
                    g_free(decomp);
                }
            }
            fclose(in);
            fclose(out);
            g_free(query);
            return dest_path;
        }
    }
    g_free(query);
    return NULL;
}

static gboolean mdd_has(ResourceReader *reader, const char *name) {
    if (!reader || !name) return FALSE;
    MddBackend *mdd = resource_reader_get_backend(reader);
    if (!mdd) return FALSE;

    char *query = g_strdup(name);
    mdx_normalize_resource_path(query);
    gboolean res = FALSE;

    for (size_t f = 0; f < mdd->num_files; f++) {
        MddFileState *fs = mdd->files[f];
        if (mdd_find_resource(fs, query, NULL, NULL)) {
            res = TRUE;
            break;
        }
    }
    g_free(query);
    return res;
}

static void mdd_close(ResourceReader *reader) {
    MddBackend *mdd = resource_reader_get_backend(reader);
    if (!mdd) return;
    for (size_t f = 0; f < mdd->num_files; f++) {
        MddFileState *fs = mdd->files[f];
        g_free(fs->mdd_path);
        g_free(fs->rbis);
        if (fs->kbis) {
            for (size_t i = 0; i < fs->nkb; i++) {
                g_free(fs->kbis[i].head_word);
                g_free(fs->kbis[i].tail_word);
            }
            g_free(fs->kbis);
        }
        g_free(fs);
    }
    g_free(mdd->files);
    g_free(mdd);
}

static ResourceReader* mdx_open_mdd_reader(GPtrArray *mdd_paths, const char *extract_dir, int is_v2, int num_size, int encoding_is_utf16, int encrypted, volatile gint *cancel_flag, gint expected) {
    if (!mdd_paths || mdd_paths->len == 0) return NULL;
    
    MddBackend *mdd = g_new0(MddBackend, 1);
    mdd->files = g_new0(MddFileState*, mdd_paths->len);

    for (guint pidx = 0; pidx < mdd_paths->len; pidx++) {
        const char *mdd_path = g_ptr_array_index(mdd_paths, pidx);
        FILE *f = fopen(mdd_path, "rb");
        if (!f) continue;

        unsigned char b4[4];
        if (fread(b4, 1, 4, f) != 4) { fclose(f); continue; }
        uint32_t hts = ru32be(b4);

        int mdd_is_v2 = is_v2, mdd_num_size = num_size, mdd_encoding_is_utf16 = encoding_is_utf16, mdd_encrypted = encrypted;
        if (hts <= 10 * 1024 * 1024) {
            unsigned char *header_raw = g_malloc(hts);
            if (header_raw && fread(header_raw, 1, hts, f) == hts) {
                size_t ascii_len = hts / 2;
                char *ascii_hdr = g_malloc(ascii_len + 1);
                if (ascii_hdr) {
                    for (size_t i = 0; i < ascii_len; i++) ascii_hdr[i] = header_raw[i * 2];
                    ascii_hdr[ascii_len] = '\0';
                    char *vp = strstr(ascii_hdr, "GeneratedByEngineVersion=\"");
                    if (vp) {
                        const char *vptr = strchr(vp, '"');
                        if (vptr) {
                            double ver = atof(vptr + 1);
                            mdd_is_v2 = (ver >= 2.0);
                            mdd_num_size = mdd_is_v2 ? 8 : 4;
                        }
                    }
                    char *ep = strstr(ascii_hdr, "Encoding=\"");
                    if (ep) {
                        const char *encoding_start = strchr(ep, '"');
                        if (encoding_start) {
                            char *encoding = g_strndup(encoding_start + 1, strchr(encoding_start + 1, '"') - encoding_start - 1);
                            if (strlen(encoding) > 0) {
                                mdd_encoding_is_utf16 = (g_ascii_strcasecmp(encoding, "UTF-16") == 0 || g_ascii_strcasecmp(encoding, "UTF16") == 0);
                            }
                            g_free(encoding);
                        }
                    }
                    mdd_encoding_is_utf16 = 1; // MDD keywords are ALWAYS UTF-16
                    char *xp = strstr(ascii_hdr, "Encrypted=\"");
                    if (xp) mdd_encrypted = atoi(xp + 11);
                    g_free(ascii_hdr);
                }
            }
            g_free(header_raw);
        } else {
            fseek(f, hts, SEEK_CUR);
        }

        fseek(f, 4, SEEK_CUR);
        int kbh_size = mdd_is_v2 ? (mdd_num_size * 5) : (mdd_num_size * 4);
        unsigned char *kbh = g_malloc(kbh_size);
        if (!kbh || fread(kbh, 1, kbh_size, f) != (size_t)kbh_size) { g_free(kbh); fclose(f); continue; }
        
        const unsigned char *kp = kbh;
        uint64_t num_key_blocks = read_num(&kp, mdd_num_size);
        uint64_t num_entries = read_num(&kp, mdd_num_size);
        uint64_t kbi_decomp = mdd_is_v2 ? read_num(&kp, mdd_num_size) : 0;
        uint64_t kbi_comp = read_num(&kp, mdd_num_size);
        uint64_t kb_data_size = read_num(&kp, mdd_num_size);
        g_free(kbh);
        
        if (mdd_is_v2) fseek(f, 4, SEEK_CUR);
        unsigned char *kbi_raw = g_malloc(kbi_comp);
        if (!kbi_raw || fread(kbi_raw, 1, kbi_comp, f) != kbi_comp) { g_free(kbi_raw); fclose(f); continue; }
        
        long kb_data_pos = ftell(f);
        MDD_KBI *kbis = g_malloc0_n(num_key_blocks, sizeof(MDD_KBI));
        if (!kbis) { g_free(kbi_raw); fclose(f); continue; }
        
        size_t kbc = 0;
        uint64_t current_file_offset = kb_data_pos;
        
        if (mdd_is_v2) {
            if (mdd_encrypted & 2) mdx_decrypt_key_block_info(kbi_raw, kbi_comp);
            size_t dlen = 0;
            unsigned char *data = mdx_block_decompress(kbi_raw, kbi_comp, kbi_decomp, &dlen);
            if (data) {
                const unsigned char *ip = data, *ie = data + dlen;
                while (ip < ie && kbc < num_key_blocks) {
                    if (ip + mdd_num_size > ie) break;
                    ip += mdd_num_size;
                    
                    uint32_t head_size = read_u8or16(&ip, 1);
                    char *head_word = NULL;
                    if (head_size > 0) {
                        char word[1024];
                        if (mdd_encoding_is_utf16) {
                            utf16le_to_utf8(ip, head_size * 2, word, sizeof(word)-1);
                        } else {
                            size_t wl = head_size; if (wl > 1023) wl = 1023;
                            memcpy(word, ip, wl); word[wl] = '\0';
                        }
                        mdx_normalize_resource_path(word);
                        head_word = g_strdup(word);
                    }
                    ip += (mdd_encoding_is_utf16 ? (head_size + 1) * 2 : (head_size + 1));
                    
                    uint32_t tail_size = read_u8or16(&ip, 1);
                    char *tail_word = NULL;
                    if (tail_size > 0) {
                        char word[1024];
                        if (mdd_encoding_is_utf16) {
                            utf16le_to_utf8(ip, tail_size * 2, word, sizeof(word)-1);
                        } else {
                            size_t wl = tail_size; if (wl > 1023) wl = 1023;
                            memcpy(word, ip, wl); word[wl] = '\0';
                        }
                        mdx_normalize_resource_path(word);
                        tail_word = g_strdup(word);
                    }
                    ip += (mdd_encoding_is_utf16 ? (tail_size + 1) * 2 : (tail_size + 1));
                    
                    if (ip + mdd_num_size * 2 > ie) {
                        g_free(head_word); g_free(tail_word); break;
                    }
                    kbis[kbc].comp_size = read_num(&ip, mdd_num_size);
                    kbis[kbc].decomp_size = read_num(&ip, mdd_num_size);
                    kbis[kbc].head_word = head_word ? head_word : g_strdup("");
                    kbis[kbc].tail_word = tail_word ? tail_word : g_strdup("");
                    kbis[kbc].file_offset = current_file_offset;
                    current_file_offset += kbis[kbc].comp_size;
                    
                    kbc++;
                }
                g_free(data);
            }
        } else {
            const unsigned char *ip = kbi_raw, *ie = kbi_raw + kbi_comp;
            while (ip < ie && kbc < num_key_blocks) {
                ip += mdd_num_size;
                
                uint32_t head_size = read_u8or16(&ip, 0);
                char *head_word = NULL;
                if (head_size > 0) {
                    char word[1024];
                    if (mdd_encoding_is_utf16) {
                        utf16le_to_utf8(ip, head_size * 2, word, sizeof(word)-1);
                    } else {
                        size_t wl = head_size; if (wl > 1023) wl = 1023;
                        memcpy(word, ip, wl); word[wl] = '\0';
                    }
                    mdx_normalize_resource_path(word);
                    head_word = g_strdup(word);
                }
                ip += head_size; // v1 doesn't have trailing NUL
                
                uint32_t tail_size = read_u8or16(&ip, 0);
                char *tail_word = NULL;
                if (tail_size > 0) {
                    char word[1024];
                    if (mdd_encoding_is_utf16) {
                        utf16le_to_utf8(ip, tail_size * 2, word, sizeof(word)-1);
                    } else {
                        size_t wl = tail_size; if (wl > 1023) wl = 1023;
                        memcpy(word, ip, wl); word[wl] = '\0';
                    }
                    mdx_normalize_resource_path(word);
                    tail_word = g_strdup(word);
                }
                ip += tail_size; // v1 doesn't have trailing NUL
                
                if (ip + mdd_num_size * 2 > ie) {
                    g_free(head_word); g_free(tail_word); break;
                }
                kbis[kbc].comp_size = read_num(&ip, mdd_num_size);
                kbis[kbc].decomp_size = read_num(&ip, mdd_num_size);
                kbis[kbc].head_word = head_word ? head_word : g_strdup("");
                kbis[kbc].tail_word = tail_word ? tail_word : g_strdup("");
                kbis[kbc].file_offset = current_file_offset;
                current_file_offset += kbis[kbc].comp_size;
                
                kbc++;
            }
        }
        g_free(kbi_raw);
        
        fseek(f, kb_data_pos + kb_data_size, SEEK_SET);
        unsigned char rbh[64]; if(fread(rbh, 1, mdd_num_size * 4, f) != (size_t)(mdd_num_size * 4)) {}
        const unsigned char *rp = rbh;
        uint64_t nrb = read_num(&rp, mdd_num_size);
        read_num(&rp, mdd_num_size);
        read_num(&rp, mdd_num_size);
        
        MDD_RBI *rbis = g_malloc0_n(nrb, sizeof(MDD_RBI));
        if (rbis) {
            uint64_t current_file_offset = ftell(f) + nrb * mdd_num_size * 2;
            uint64_t current_decomp_offset = 0;
            
            for (uint64_t i = 0; i < nrb; i++) {
                unsigned char p[16]; if(fread(p, 1, mdd_num_size * 2, f) != (size_t)(mdd_num_size * 2)) break;
                const unsigned char *pp = p;
                rbis[i].comp = read_num(&pp, mdd_num_size);
                rbis[i].decomp = read_num(&pp, mdd_num_size);
                rbis[i].file_offset = current_file_offset;
                rbis[i].decomp_offset = current_decomp_offset;
                
                current_file_offset += rbis[i].comp;
                current_decomp_offset += rbis[i].decomp;
            }
            
            MddFileState *fs = g_new0(MddFileState, 1);
            fs->mdd_path = g_strdup(mdd_path);
            fs->is_v2 = mdd_is_v2;
            fs->num_size = mdd_num_size;
            fs->encoding_is_utf16 = mdd_encoding_is_utf16;
            fs->encrypted = mdd_encrypted;
            fs->kb_data_pos = kb_data_pos;
            fs->kb_data_size = kb_data_size;
            fs->rbis = rbis;
            fs->nrb = nrb;
            fs->total_decomp = current_decomp_offset;
            fs->kbis = kbis;
            fs->nkb = kbc;
            
            mdd->files[mdd->num_files++] = fs;
        } else {
            for(size_t i=0; i<kbc; i++) {
                g_free(kbis[i].head_word);
                g_free(kbis[i].tail_word);
            }
            g_free(kbis);
        }
        fclose(f);
    }
    
    printf("[ResourceReader] Indexed %zu MDD files (lazy extraction)\n", mdd->num_files);
    
    return resource_reader_new(extract_dir, mdd, mdd_get, mdd_has, mdd_close);
}

static char *mdx_prepare_resource_dir(const char *path, int is_v2, int num_size, int encoding_is_utf16, int encrypted, volatile gint *cancel_flag, gint expected, ResourceReader **out_reader) {
    char *mdx_dir = g_path_get_dirname(path);
    char *mdx_basename = g_path_get_basename(path);
    char *dot_pos = strrchr(mdx_basename, '.');
    if (dot_pos) *dot_pos = '\0';

    const char *cache_base = dict_cache_base_dir();
    char *resource_dir = g_build_filename(cache_base, "diction", "resources", mdx_basename, NULL);

    g_mkdir_with_parents(resource_dir, 0755);

    GPtrArray *mdd_paths = mdx_collect_mdd_paths(path);
    if (!mdd_paths || mdd_paths->len == 0) {
        g_free(mdx_dir); g_free(mdx_basename);
        if (mdd_paths) g_ptr_array_free(mdd_paths, TRUE);
        return resource_dir;
    }

    if (out_reader) {
        *out_reader = mdx_open_mdd_reader(mdd_paths, resource_dir, is_v2, num_size, encoding_is_utf16, encrypted, cancel_flag, expected);
    }

    g_ptr_array_free(mdd_paths, TRUE);
    g_free(mdx_dir); g_free(mdx_basename);
    return resource_dir;
}


/* Convert a BMP or ICO icon file to PNG using GdkPixbuf.
 * Returns a newly allocated path to the PNG file, or NULL on failure.
 * The caller should free the returned string with g_free(). */
static char *mdx_convert_icon_to_png(const char *icon_path) {
    if (!icon_path) return NULL;

    /* Only convert formats WebKit's <img> can't render */
    gboolean is_bmp = g_str_has_suffix(icon_path, ".bmp") || g_str_has_suffix(icon_path, ".BMP");
    gboolean is_ico = g_str_has_suffix(icon_path, ".ico") || g_str_has_suffix(icon_path, ".ICO");
    if (!is_bmp && !is_ico) return NULL;

    /* Build the target PNG path: strip extension, add .png */
    char *base = g_strdup(icon_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char *png_path = g_strconcat(base, ".png", NULL);
    g_free(base);

    /* If already converted, reuse */
    if (g_file_test(png_path, G_FILE_TEST_EXISTS)) {
        return png_path;
    }

    GError *err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(icon_path, &err);
    if (!pixbuf) {
        fprintf(stderr, "[MDX] Icon conversion failed (load): %s — %s\n",
                icon_path, err ? err->message : "unknown");
        g_clear_error(&err);
        g_free(png_path);
        return NULL;
    }

    if (!gdk_pixbuf_save(pixbuf, png_path, "png", &err, NULL)) {
        fprintf(stderr, "[MDX] Icon conversion failed (save): %s — %s\n",
                png_path, err ? err->message : "unknown");
        g_clear_error(&err);
        g_object_unref(pixbuf);
        g_free(png_path);
        return NULL;
    }

    g_object_unref(pixbuf);
    fprintf(stderr, "[MDX] Converted icon to PNG: %s\n", png_path);
    return png_path;
}


static void mdx_detect_icon(DictMmap *dict, const char *path) {
    if (!dict->resource_dir) return;

    /* 1. Check for common icons in MDD resources via reader */
    if (dict->resource_reader) {
        char *basename = g_path_get_basename(path);
        char *dot = strrchr(basename, '.');
        if (dot) *dot = '\0';

        char *name1 = g_strdup_printf("%s.png", basename);
        char *name2 = g_strdup_printf("%s.ico", basename);
        char *name3 = g_strdup_printf("%s.jpg", basename);
        char *name4 = g_strdup_printf("/%s.png", basename);

        const char *icon_names[] = {
            name1, name2, name3, name4,
            "icon.png", "icon.ico", "icon.jpg", "icon.bmp",
            "/icon.png", "/icon.ico", "logo.png", "/logo.png", NULL
        };

        for (int i = 0; icon_names[i]; i++) {
            if (resource_reader_has(dict->resource_reader, icon_names[i])) {
                dict->icon_path = resource_reader_get(dict->resource_reader, icon_names[i]);
                if (dict->icon_path) break;
            }
        }
        g_free(name1); g_free(name2); g_free(name3); g_free(name4);
        g_free(basename);
    }

    /* 2. Check for the specially extracted mdx_icon from MDX records */
    if (!dict->icon_path) {
        const char *mdx_icon_exts[] = {"png", "ico", "jpg", "jpeg", "bmp", NULL};
        for (int i = 0; mdx_icon_exts[i]; i++) {
            char *mdx_icon_filename = g_strdup_printf("mdx_icon.%s", mdx_icon_exts[i]);
            char *mdx_icon_path = g_build_filename(dict->resource_dir, mdx_icon_filename, NULL);
            if (g_file_test(mdx_icon_path, G_FILE_TEST_EXISTS)) {
                dict->icon_path = mdx_icon_path;
                g_free(mdx_icon_filename);
                break;
            }
            g_free(mdx_icon_filename);
            g_free(mdx_icon_path);
        }
    }

    /* 3. Convert BMP/ICO icons to PNG so WebKit can render them in <img> tags */
    if (dict->icon_path) {
        char *png_path = mdx_convert_icon_to_png(dict->icon_path);
        if (png_path) {
            g_free(dict->icon_path);
            dict->icon_path = png_path;
        }
    }
}

static int cmp_tree_entry_doff(const void *a, const void *b) {
    const FlatTreeEntry *ea = a;
    const FlatTreeEntry *eb = b;
    if (ea->d_off < eb->d_off) return -1;
    if (ea->d_off > eb->d_off) return 1;
    return 0;
}

static gboolean mdx_is_valid_header(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;

    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return FALSE;
    }

    unsigned char buf4[4];
    if (fread(buf4, 1, 4, f) != 4) {
        fclose(f);
        return FALSE;
    }

    uint32_t header_text_size = ((uint32_t)buf4[0] << 24) | ((uint32_t)buf4[1] << 16) |
                               ((uint32_t)buf4[2] << 8) | buf4[3];

    if (header_text_size < 10 || header_text_size > 10 * 1024 * 1024) {
        fclose(f);
        return FALSE;
    }

    if ((guint64)st.st_size < 4 + (guint64)header_text_size + 4) {
        fclose(f);
        return FALSE;
    }

    unsigned char *header_raw = g_malloc(header_text_size);
    if (!header_raw) {
        fclose(f);
        return FALSE;
    }

    if (fread(header_raw, 1, header_text_size, f) != header_text_size) {
        g_free(header_raw);
        fclose(f);
        return FALSE;
    }

    fclose(f);

    char *utf8_hdr = NULL;
    GError *err = NULL;
    utf8_hdr = g_convert((const char*)header_raw, header_text_size, "UTF-8", "UTF-16LE", NULL, NULL, &err);
    if (!utf8_hdr || !strstr(utf8_hdr, "Encoding=")) {
        if (err) { g_error_free(err); err = NULL; }
        g_free(utf8_hdr);
        if (g_utf8_validate((const char*)header_raw, header_text_size, NULL)) {
            utf8_hdr = g_strndup((const char*)header_raw, header_text_size);
        } else {
            utf8_hdr = g_convert((const char*)header_raw, header_text_size, "UTF-8", "ISO-8859-1", NULL, NULL, &err);
        }
    }
    if (err) g_error_free(err);
    g_free(header_raw);

    if (!utf8_hdr) {
        return FALSE;
    }

    char *v_ver = extract_header_attribute(utf8_hdr, "GeneratedByEngineVersion");
    char *title = extract_header_attribute(utf8_hdr, "Title");
    gboolean has_dict_tag = (strstr(utf8_hdr, "<Dictionary") != NULL || strstr(utf8_hdr, "<dictionary") != NULL);
    gboolean is_valid = has_dict_tag && (v_ver != NULL || title != NULL);
    g_free(v_ver);
    g_free(title);
    g_free(utf8_hdr);

    return is_valid;
}

typedef struct MdxRecordBlock {
    uint64_t comp_size;
    uint64_t decomp_size;
    uint64_t file_offset;      
    uint64_t uncomp_offset;    
} MdxRecordBlock;

typedef struct MdxContext {
    int fd;
    MdxRecordBlock *rbs;
    uint64_t nrb;
    int is_utf16;
    char *encoding;
} MdxContext;

static MdxContext* mdx_init_context(FILE *fh, int is_v2, int num_size, uint64_t header_text_size, int encoding_is_utf16, const char *dict_encoding) {
    if (!fh) return NULL;
    clearerr(fh);
    long orig_pos = ftell(fh);
    if (fseek(fh, 4 + header_text_size + 4, SEEK_SET) != 0) {
        return NULL;
    }

    int kbh_size = is_v2 ? (num_size * 5) : (num_size * 4);
    unsigned char *kbh = g_malloc(kbh_size);
    if (fread(kbh, 1, kbh_size, fh) != (size_t)kbh_size) {
        g_free(kbh); fseek(fh, orig_pos, SEEK_SET); return NULL;
    }
    const unsigned char *kp = kbh;
    uint64_t num_key_blocks = read_num(&kp, num_size); (void)num_key_blocks;
    uint64_t num_entries = read_num(&kp, num_size); (void)num_entries;
    if (is_v2) read_num(&kp, num_size);
    uint64_t kbi_comp = read_num(&kp, num_size);
    uint64_t kb_data_size = read_num(&kp, num_size);
    g_free(kbh);

    if (fseek(fh, 4 + header_text_size + 4 + kbh_size + (is_v2?4:0) + kbi_comp + kb_data_size, SEEK_SET) != 0) {
        fseek(fh, orig_pos, SEEK_SET);
        return NULL;
    }

    unsigned char rbh[64];
    if (fread(rbh, 1, num_size * 4, fh) != (size_t)(num_size * 4)) {
        fseek(fh, orig_pos, SEEK_SET);
        return NULL;
    }
    const unsigned char *rp = rbh;
    uint64_t nrb = read_num(&rp, num_size);
    read_num(&rp, num_size);
    read_num(&rp, num_size);

    MdxContext *ctx = g_new0(MdxContext, 1);
    ctx->fd = dup(fileno(fh));
    if (ctx->fd < 0) {
        g_free(ctx);
        fseek(fh, orig_pos, SEEK_SET);
        return NULL;
    }
    ctx->nrb = nrb;
    ctx->is_utf16 = encoding_is_utf16;
    ctx->encoding = g_strdup(dict_encoding);
    ctx->rbs = g_malloc0_n(nrb, sizeof(MdxRecordBlock));

    uint64_t file_offset = 4 + header_text_size + 4 + kbh_size + (is_v2?4:0) + kbi_comp + kb_data_size + num_size*4 + nrb * num_size * 2;
    uint64_t uncomp_offset = 0;

    for (uint64_t i = 0; i < nrb; i++) {
        unsigned char tmp[16];
        if (fread(tmp, 1, num_size * 2, fh) != (size_t)(num_size * 2)) {
            close(ctx->fd);
            g_free(ctx->encoding);
            g_free(ctx->rbs);
            g_free(ctx);
            fseek(fh, orig_pos, SEEK_SET);
            return NULL;
        }
        const unsigned char *ppb = tmp;
        uint64_t comp = read_num(&ppb, num_size);
        uint64_t decomp = read_num(&ppb, num_size);
        
        ctx->rbs[i].comp_size = comp;
        ctx->rbs[i].decomp_size = decomp;
        ctx->rbs[i].file_offset = file_offset;
        ctx->rbs[i].uncomp_offset = uncomp_offset;
        
        file_offset += comp;
        uncomp_offset += decomp;
    }
    
    fseek(fh, orig_pos, SEEK_SET);
    return ctx;
}

const char* mdx_get_definition_on_the_fly(DictMmap *dict, const FlatTreeEntry *entry, size_t *out_len, char **out_to_free) {
    if (!dict || !dict->mdx_ctx || !entry) {
        return NULL;
    }
    MdxContext *ctx = dict->mdx_ctx;
    
    if (out_len) *out_len = 0;
    if (out_to_free) *out_to_free = NULL;
    
    uint64_t target_off = entry->d_off;
    int l = 0, r = ctx->nrb - 1;
    int block_idx = -1;
    while (l <= r) {
        int m = l + (r - l) / 2;
        if (target_off >= ctx->rbs[m].uncomp_offset && 
            target_off < ctx->rbs[m].uncomp_offset + ctx->rbs[m].decomp_size) {
            block_idx = m;
            break;
        }
        if (target_off < ctx->rbs[m].uncomp_offset) {
            r = m - 1;
        } else {
            l = m + 1;
        }
    }
    if (block_idx == -1) return NULL;
    
    unsigned char *comp = g_malloc(ctx->rbs[block_idx].comp_size);
    ssize_t bytes_read = pread(ctx->fd, comp, ctx->rbs[block_idx].comp_size, ctx->rbs[block_idx].file_offset);
    if (bytes_read != (ssize_t)ctx->rbs[block_idx].comp_size) {
        g_free(comp);
        return NULL;
    }
    
    size_t dlen = 0;
    unsigned char *data = mdx_block_decompress(comp, ctx->rbs[block_idx].comp_size, ctx->rbs[block_idx].decomp_size, &dlen);
    g_free(comp);
    if (!data) return NULL;
    
    uint64_t rel_off = target_off - ctx->rbs[block_idx].uncomp_offset;
    uint64_t rec_len = entry->d_len;

    if (rel_off >= dlen) {
        g_free(data);
        return NULL;
    }
    if (rel_off + rec_len > dlen) rec_len = dlen - rel_off;
    
    const unsigned char *rec_ptr = data + rel_off;
    char *utf8_def = NULL;
    size_t def_len = 0;

    if (ctx->is_utf16) {
        size_t rlen = rec_len;
        if (rlen >= 2 && rec_ptr[rlen-1] == 0 && rec_ptr[rlen-2] == 0) rlen -= 2;
        size_t max_out = rlen * 2;
        utf8_def = g_malloc(max_out + 1);
        def_len = convert_utf16le_to_utf8(rec_ptr, rlen, (unsigned char*)utf8_def, NULL);
        utf8_def[def_len] = '\0';
    } else {
        size_t rlen = rec_len;
        if (rlen > 0 && rec_ptr[rlen-1] == 0) rlen--;
        if (ctx->encoding) {
            utf8_def = g_convert((const char*)rec_ptr, rlen, "UTF-8", ctx->encoding, NULL, &def_len, NULL);
        } else {
            utf8_def = g_strndup((const char*)rec_ptr, rlen);
            def_len = rlen;
        }
    }
    
    g_free(data);
    
    if (out_len) *out_len = def_len;
    if (out_to_free) *out_to_free = utf8_def;
    return utf8_def;
}

void mdx_free_context(void *ctx_ptr) {
    if (!ctx_ptr) return;
    MdxContext *ctx = ctx_ptr;
    if (ctx->fd >= 0) close(ctx->fd);
    g_free(ctx->rbs);
    g_free(ctx->encoding);
    g_free(ctx);
}

DictMmap *parse_mdx_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    if (!mdx_is_valid_header(path)) {
        fprintf(stderr, "[MDX] Ignoring invalid/non-dictionary MDX file: %s\n", path);
        return NULL;
    }
    dict_cache_ensure_dir();

    char *cache_path = dict_cache_path_for(path);
    gboolean cache_valid = (access(cache_path, F_OK) == 0) &&
                           dict_cache_is_valid(cache_path, path);

    int cache_fd = -1;
    const char *dict_data = NULL;
    size_t dict_size = 0;
    char *title = NULL;
    char *stylesheet = NULL;
    char *s_lang = NULL;
    char *t_lang = NULL;
    char *source_dir = g_path_get_dirname(path);

    /* ── read header ── */
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return NULL; // Prevent fseek crashes during EMFILE (ulimit -n) handling
    }

    int is_v2 = 0, num_size = 4, encoding_is_utf16 = 0, encrypted = 0;
    char *dict_encoding = NULL;
    uint32_t header_text_size = 0;

    unsigned char buf4[4];
    if (fread(buf4, 1, 4, fh) == 4) {
        header_text_size = ru32be(buf4);
        if (header_text_size <= 10*1024*1024) {
            unsigned char *header_raw = g_malloc(header_text_size);
            if (header_raw && fread(header_raw, 1, header_text_size, fh) == header_text_size) {
                char *utf8_hdr = NULL;
                /* Try UTF-16LE first as it's common for v2.0+ */
                GError *err = NULL;
                utf8_hdr = g_convert((const char*)header_raw, header_text_size, "UTF-8", "UTF-16LE", NULL, NULL, &err);
                if (!utf8_hdr || !strstr(utf8_hdr, "Encoding=")) {
                    if (err) { g_error_free(err); err = NULL; }
                    g_free(utf8_hdr);
                    /* Fallback to UTF-8/Latined headers */
                    if (g_utf8_validate((const char*)header_raw, header_text_size, NULL)) {
                        utf8_hdr = g_strndup((const char*)header_raw, header_text_size);
                    } else {
                        utf8_hdr = g_convert((const char*)header_raw, header_text_size, "UTF-8", "ISO-8859-1", NULL, NULL, &err);
                    }
                }
                if (err) g_error_free(err);

                if (utf8_hdr) {
                    title = extract_header_attribute(utf8_hdr, "Title");
                    if (title) {
                        char *stripped = strip_html_tags(title);
                        g_free(title);
                        if (!stripped || strcmp(stripped, "Title (No HTML code allowed)") == 0 || strlen(stripped) == 0) {
                            g_free(stripped);
                            title = NULL;
                        } else {
                            title = stripped;
                            fprintf(stderr, "[MDX DEBUG] Title: '%s'\n", title);
                        }
                    }

                    if (!title) {
                        char *basename = g_path_get_basename(path);
                        char *dot = strrchr(basename, '.');
                        if (dot) *dot = '\0';
                        title = basename;
                        fprintf(stderr, "[MDX DEBUG] Fallback Title from filename: '%s'\n", title);
                    }

                    char *v_ver = extract_header_attribute(utf8_hdr, "GeneratedByEngineVersion");
                    if (v_ver) {
                        double ver = atof(v_ver);
                        is_v2 = (ver >= 2.0);
                        num_size = is_v2 ? 8 : 4;
                        g_free(v_ver);
                    }

                    char *enc_raw = extract_header_attribute(utf8_hdr, "Encoding");
                    if (enc_raw && strlen(enc_raw) > 0) {
                        if (g_ascii_strcasecmp(enc_raw, "UTF-16") == 0 || g_ascii_strcasecmp(enc_raw, "UTF16") == 0) {
                            encoding_is_utf16 = 1;
                        } else if (g_ascii_strcasecmp(enc_raw, "UTF-8") == 0 || g_ascii_strcasecmp(enc_raw, "UTF8") == 0) {
                            encoding_is_utf16 = 0;
                        } else {
                            dict_encoding = g_strdup(enc_raw);
                            encoding_is_utf16 = 0;
                        }
                    }
                    g_free(enc_raw);

                    char *xp = extract_header_attribute(utf8_hdr, "Encrypted");
                    if (xp) {
                        encrypted = atoi(xp);
                        g_free(xp);
                    }

                    stylesheet = extract_header_attribute(utf8_hdr, "StyleSheet");
                    
                    char *raw_s_lang = extract_header_attribute(utf8_hdr, "SourceLanguage");
                    char *raw_t_lang = extract_header_attribute(utf8_hdr, "TargetLanguage");
                    s_lang = langpair_normalize_language_name(raw_s_lang);
                    t_lang = langpair_normalize_language_name(raw_t_lang);
                    g_free(raw_s_lang);
                    g_free(raw_t_lang);
                    if (!s_lang && t_lang) {
                        s_lang = g_strdup(t_lang);
                    } else if (!t_lang && s_lang) {
                        t_lang = g_strdup(s_lang);
                    }

                    g_free(utf8_hdr);
                } else {
                    g_free(header_raw);
                    if (fh) fclose(fh);
                    return NULL;
                }
                g_free(header_raw);
            } else {
                g_free(header_raw);
                if (fh) fclose(fh);
                return NULL;
            }
        } else {
            if (fh) fclose(fh);
            return NULL;
        }
    } else {
        if (fh) fclose(fh);
        return NULL;
    }

    if (!title || (!is_v2 && num_size != 4 && num_size != 8)) {
        // Not a valid MDict MDX file (e.g. React Markdown .mdx)
        g_free(title);
        g_free(stylesheet);
        g_free(s_lang);
        g_free(t_lang);
        if (fh) fclose(fh);
        return NULL;
    }

    /* ───────────────────────────── */
    /* FAST PATH: use cache directly */
    /* ───────────────────────────── */
    if (cache_valid) {
        cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd < 0) {
            cache_valid = FALSE;
            goto rebuild_cache;
        }
        struct stat st;
        if (fstat(cache_fd, &st) != 0 || st.st_size < 16) {
            close(cache_fd);
            cache_valid = FALSE;
            goto rebuild_cache;
        }

        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
        if (dict_data == MAP_FAILED) {
            close(cache_fd);
            cache_valid = FALSE;
            goto rebuild_cache;
        }
        close(cache_fd);

        DictMmap *dict = g_new0(DictMmap, 1);
        dict->fd = -1; // Explicitly invalidate fd since mmap handles the memory mapping autonomously
        dict->data = dict_data;
        dict->size = dict_size;
        dict->name = title;
        dict->source_dir = g_strdup(source_dir);
        dict->mdx_stylesheet = stylesheet ? g_strdup(stylesheet) : NULL;
        dict->source_lang = s_lang;
        dict->target_lang = t_lang;
        dict->index = flat_index_open(dict->data, dict->size);

        /* flat_index_open already reads the index from end of file.
         * Just validate it. */
        if (dict->index && dict->index->count > 0) {
            if (!flat_index_validate(dict->index)) {
                fprintf(stderr, "[MDX] Cache index validation failed for %s — rebuilding index.\n", path);
                flat_index_close(dict->index);
                dict->index = g_new0(FlatIndex, 1);
                dict->index->mmap_data = dict->data;
                dict->index->mmap_size = dict->size;
            }
        }

        if (dict_cache_is_compressed(dict->data, dict->size)) {
            const DictCacheHeader *hdr = (const DictCacheHeader*)dict->data;
            if (hdr->chunk_count > 0) {
                dict->is_compressed = TRUE;
                dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, hdr);
            }
        }
        
        if (!dict->is_compressed) {
            dict->mdx_ctx = mdx_init_context(fh, is_v2, num_size, header_text_size, encoding_is_utf16, dict_encoding);
        }

        if (!(cancel_flag && g_atomic_int_get(cancel_flag) != expected)) {
            dict->resource_dir = mdx_prepare_resource_dir(path, is_v2, num_size, encoding_is_utf16, encrypted, cancel_flag, expected, &dict->resource_reader);
            mdx_detect_icon(dict, path);
        }

        g_free(stylesheet);
        g_free(source_dir);
        g_free(cache_path);
        if (fh) fclose(fh);
        return dict;
    }

    /* ───────────────────────────── */
    /* BUILD CACHE (ZERO-COPY)       */
    /* ───────────────────────────── */
rebuild_cache:

    struct stat src_st;
    guint64 cache_bytes_hint = (stat(path, &src_st) == 0 && src_st.st_size > 0)
        ? (guint64) src_st.st_size
        : 0;
    if (!dict_cache_prepare_target_path(cache_path, cache_bytes_hint)) {
        if (fh) fclose(fh);
        g_free(cache_path);
        g_free(stylesheet);
        g_free(title);
        g_free(s_lang);
        g_free(t_lang);
        g_free(source_dir);
        g_free(dict_encoding);
        return NULL;
    }

    fseek(fh, 4 + header_text_size + 4, SEEK_SET); // skip HeaderLen + Header + Checksum

    int kbh_size = is_v2 ? (num_size * 5) : (num_size * 4);
    unsigned char *kbh = g_malloc(kbh_size);
    if (fread(kbh, 1, kbh_size, fh) != (size_t)kbh_size) {
        g_free(kbh);
        if (fh) fclose(fh);
        g_free(cache_path);
        g_free(stylesheet);
        g_free(title);
        g_free(s_lang);
        g_free(t_lang);
        g_free(source_dir);
        g_free(dict_encoding);
        return NULL;
    }

    const unsigned char *kp = kbh;
    uint64_t num_key_blocks = read_num(&kp, num_size);
    (void)num_key_blocks;
    uint64_t num_entries = read_num(&kp, num_size);
    uint64_t kbi_decomp = is_v2 ? read_num(&kp, num_size) : 0;
    uint64_t kbi_comp = read_num(&kp, num_size);
    uint64_t kb_data_size = read_num(&kp, num_size);
    g_free(kbh);
    
    // Sanity check num_entries to prevent 50+ GB allocations
    if (num_entries > 100000000) {
        if (fh) fclose(fh);
        g_free(cache_path);
        g_free(stylesheet);
        g_free(title);
        g_free(s_lang);
        g_free(t_lang);
        g_free(source_dir);
        g_free(dict_encoding);
        return NULL;
    }

#include "dict-cache-builder.h"

    DictCacheBuilder *builder = dict_cache_builder_new(cache_path, num_entries);
    if (!builder) {
        if (fh) fclose(fh);
        g_free(cache_path);
        g_free(stylesheet);
        g_free(title);
        g_free(s_lang);
        g_free(t_lang);
        g_free(source_dir);
        g_free(dict_encoding);
        return NULL;
    }

    if (is_v2) fseek(fh, 4, SEEK_CUR);
    unsigned char *kbi_raw = g_malloc(kbi_comp);
    if (fread(kbi_raw, 1, kbi_comp, fh) != kbi_comp) {
        g_free(kbi_raw);
        dict_cache_builder_free(builder);
        if (fh) fclose(fh);
        g_free(cache_path);
        g_free(stylesheet);
        g_free(title);
        g_free(s_lang);
        g_free(t_lang);
        g_free(source_dir);
        g_free(dict_encoding);
        return NULL;
    }

    FlatTreeEntry *tree_entries = g_malloc0_n(num_entries, sizeof(FlatTreeEntry));
    size_t valid_count = 0;

    size_t kbi_dlen = 0;
    unsigned char *kbi_data = NULL;
    if (is_v2) {
        if (encrypted & 2) mdx_decrypt_key_block_info(kbi_raw, kbi_comp);
        kbi_data = mdx_block_decompress(kbi_raw, kbi_comp, kbi_decomp, &kbi_dlen);
        g_free(kbi_raw);
    } else {
        kbi_data = kbi_raw;
        kbi_dlen = kbi_comp;
    }

    uint64_t internal_icon_id = (uint64_t)-1;


    if (kbi_data) {
        const unsigned char *ip = kbi_data, *ie = kbi_data + kbi_dlen;
        while (ip < ie && valid_count < num_entries) {
            if (ip + num_size > ie) break;
            ip += num_size;
            
            if (ip >= ie) break;
            uint32_t head_size = read_u8or16(&ip, is_v2);
            size_t head_bytes = (encoding_is_utf16
                                 ? (head_size + (is_v2 ? 1 : 0)) * 2
                                 : head_size + (is_v2 ? 1 : 0));
            if (ip + head_bytes > ie) break;
            ip += head_bytes;
            
            if (ip >= ie) break;
            uint32_t tail_size = read_u8or16(&ip, is_v2);
            size_t tail_bytes = (encoding_is_utf16
                                 ? (tail_size + (is_v2 ? 1 : 0)) * 2
                                 : tail_size + (is_v2 ? 1 : 0));
            if (ip + tail_bytes > ie) break;
            ip += tail_bytes;
            
            if (ip + num_size * 2 > ie) break;
            uint64_t comp_size = read_num(&ip, num_size);
            uint64_t decomp_size = read_num(&ip, num_size);

            long next_kb = ftell(fh) + (long)comp_size;
            unsigned char *kb_comp = g_malloc(comp_size);
            if (fread(kb_comp, 1, comp_size, fh) == comp_size) {
                size_t kb_dlen = 0;
                unsigned char *kb_data = mdx_block_decompress(kb_comp, comp_size, decomp_size, &kb_dlen);
                if (kb_data) {
                    const unsigned char *kp_ent = kb_data, *ke_ent = kb_data + kb_dlen;
                    while (kp_ent < ke_ent && valid_count < num_entries) {
                        if (kp_ent + num_size > ke_ent) break;
                        uint64_t id = read_num(&kp_ent, num_size);
                        char word[1024];
                        if (encoding_is_utf16) {
                            const unsigned char *ws = kp_ent;
                            while (kp_ent + 1 < ke_ent && !(kp_ent[0] == 0 && kp_ent[1] == 0)) kp_ent += 2;
                            utf16le_to_utf8(ws, kp_ent - ws, word, sizeof(word)-1);
                            if (kp_ent + 1 < ke_ent) kp_ent += 2;
                        } else {
                            const unsigned char *ws = kp_ent;
                            while (kp_ent < ke_ent && *kp_ent != '\0') kp_ent++;
                            size_t wl = kp_ent - ws; if (wl > 1023) wl = 1023;
                            
                            if (dict_encoding && wl > 0) {
                                GError *err = NULL;
                                char *utf8 = g_convert((const char*)ws, wl, "UTF-8", dict_encoding, NULL, NULL, &err);
                                if (utf8) {
                                    size_t ulen = strlen(utf8);
                                    if (ulen > 1023) ulen = 1023;
                                    memcpy(word, utf8, ulen);
                                    word[ulen] = '\0';
                                    g_free(utf8);
                                } else {
                                    if (err) g_error_free(err);
                                    memcpy(word, ws, wl); word[wl] = '\0';
                                }
                            } else {
                                memcpy(word, ws, wl); word[wl] = '\0';
                            }
                            if (kp_ent < ke_ent) kp_ent++;
                        }

                        /* Detect internal icon entries */
                        if (internal_icon_id == (uint64_t)-1) {
                            char *basename = g_path_get_basename(path);
                            char *dot = strrchr(basename, '.');
                            if (dot) *dot = '\0';
                            
                            const char *icon_names[] = {"icon.png", "_logo_", "icon.ico", "icon.jpg", "logo.png", "logo.jpg", NULL};
                            gboolean found = FALSE;
                            for (int i = 0; icon_names[i]; i++) {
                                if (g_ascii_strcasecmp(word, icon_names[i]) == 0) {
                                    found = TRUE; break;
                                }
                            }
                            if (!found && (g_ascii_strcasecmp(basename, word) == 0)) found = TRUE;
                            
                            if (found) {
                                internal_icon_id = id;
                                fprintf(stderr, "[MDX] Found internal icon entry: '%s' at record offset %" G_GUINT64_FORMAT "\n", word, id);
                            }
                            g_free(basename);
                        }

                        size_t wlen = strlen(word);
                        uint64_t hw_off = 0;
                        dict_cache_builder_add_headword(builder, word, wlen, &hw_off);

                        tree_entries[valid_count].h_off = (uint32_t)hw_off;
                        tree_entries[valid_count].h_len = (uint32_t)wlen;
                        tree_entries[valid_count].d_off = (uint32_t)id;
                        valid_count++;
                    }
                    g_free(kb_data);
                }
            }
            g_free(kb_comp);
            fseek(fh, next_kb, SEEK_SET);
        }
        g_free(kbi_data);
    } 
    
    settings_scan_progress_notify(path, 40);

    fseek(fh, 4 + header_text_size + 4 + kbh_size + (is_v2?4:0) + kbi_comp + kb_data_size, SEEK_SET);

    unsigned char rbh[64];
    fread(rbh, 1, num_size * 4, fh);
    const unsigned char *rp = rbh;
    uint64_t nrb = read_num(&rp, num_size);
    read_num(&rp, num_size);
    read_num(&rp, num_size);

    typedef struct { uint64_t comp, decomp; } RB;
    RB *rbs = g_malloc0_n(nrb, sizeof(RB));
    for (uint64_t i = 0; i < nrb; i++) {
        unsigned char tmp[16];
        if (fread(tmp, 1, num_size * 2, fh) != (size_t)(num_size * 2)) break;
        const unsigned char *ppb = tmp;
        rbs[i].comp = read_num(&ppb, num_size);
        rbs[i].decomp = read_num(&ppb, num_size);
    }
    /* Sort by d_off to compute lengths */
    qsort(tree_entries, valid_count, sizeof(FlatTreeEntry), cmp_tree_entry_doff);

    uint64_t total_decomp = 0;
    for (uint64_t j = 0; j < nrb; j++) total_decomp += rbs[j].decomp;

    for (size_t k = 0; k < valid_count; k++) {
        uint64_t next_id = (k + 1 < valid_count) ? tree_entries[k+1].d_off : total_decomp;
        if (next_id >= tree_entries[k].d_off) {
            tree_entries[k].d_len = next_id - tree_entries[k].d_off;
        } else {
            tree_entries[k].d_len = 0;
        }
    }
    
    g_free(rbs);

    /* Sort entries for binary search */
    settings_scan_progress_notify(path, 85);
    
    /* We need to mmap the builder's file to sort. Builder keeps file open. */
    dict_cache_builder_flush(builder);
    int sort_fd = open(cache_path, O_RDONLY);
    if (sort_fd >= 0) {
        struct stat st_tmp;
        fstat(sort_fd, &st_tmp);
        void *sort_mmap = mmap(NULL, (size_t)st_tmp.st_size, PROT_READ, MAP_PRIVATE, sort_fd, 0);
        if (sort_mmap != MAP_FAILED) {
            flat_index_sort_entries(tree_entries, valid_count, sort_mmap, (size_t)st_tmp.st_size);
            munmap(sort_mmap, (size_t)st_tmp.st_size);
        }
        close(sort_fd);
    }

    settings_scan_progress_notify(path, 95);
    dict_cache_builder_finalize_index_only(builder, tree_entries, (uint64_t)valid_count, 0, NULL);
    
    if (valid_count == 0) {
        fprintf(stderr, "[MDX] Parsed zero entries for %s\n", path);
    }

    MdxContext *mdx_ctx = mdx_init_context(fh, is_v2, num_size, header_text_size, encoding_is_utf16, dict_encoding);

    dict_cache_builder_free(builder);
    const char *sources[] = { path };
    dict_cache_sync_mtime(cache_path, sources, 1);
    fclose(fh);
    g_free(dict_encoding);

    cache_fd = open(cache_path, O_RDONLY);
    if (cache_fd < 0) {
        if (mdx_ctx) mdx_free_context(mdx_ctx);
        g_free(tree_entries);
        g_free(cache_path);
        g_free(source_dir);
        g_free(stylesheet);
        g_free(title);
        g_free(s_lang);
        g_free(t_lang);
        return NULL;
    }
    struct stat st_final;
    fstat(cache_fd, &st_final);
    dict_size = st_final.st_size;
    dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (dict_data == MAP_FAILED) {
        close(cache_fd);
        if (mdx_ctx) mdx_free_context(mdx_ctx);
        g_free(tree_entries);
        g_free(cache_path);
        g_free(source_dir);
        g_free(stylesheet);
        g_free(title);
        g_free(s_lang);
        g_free(t_lang);
        return NULL;
    }

    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = cache_fd;
    dict->data = dict_data;
    dict->size = dict_size;
    dict->name = title;
    dict->source_dir = source_dir;
    dict->mdx_stylesheet = stylesheet;
    dict->mdx_ctx = mdx_ctx;
    dict->index = flat_index_open(dict->data, dict->size);

    if (dict_cache_is_compressed(dict->data, dict->size)) {
        dict->is_compressed = TRUE;
        dict->chunk_reader = dict_chunk_reader_new(dict->data, dict->size, (const DictCacheHeader*)dict->data);
    }

    g_free(tree_entries);
    g_free(cache_path);

    dict->resource_dir = mdx_prepare_resource_dir(path, is_v2, num_size, encoding_is_utf16, encrypted, cancel_flag, expected, &dict->resource_reader);
    mdx_detect_icon(dict, path);
    
    settings_scan_progress_notify(path, 100);

    return dict;
}

