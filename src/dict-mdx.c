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
#include <errno.h>

/* Cache helpers for persistent dictionary storage */

/* Validate and clean UTF-8 string */
static char* validate_utf8_string(const char *input) {
    if (!input) return NULL;
    
    /* Check if valid UTF-8 */
    if (g_utf8_validate(input, -1, NULL)) {
        return g_strdup(input);
    }
    
    /* Try to convert from Latin-1 to UTF-8 */
    GError *error = NULL;
    char *converted = g_convert(input, -1, "UTF-8", "ISO-8859-1", NULL, NULL, &error);
    if (converted) {
        return converted;
    }
    
    if (error) g_error_free(error);
    
    /* Fallback: strip invalid characters */
    GString *result = g_string_new("");
    const char *p = input;
    while (*p) {
        if ((unsigned char)*p < 128) {
            g_string_append_c(result, *p);
        }
        p++;
    }
    
    return g_string_free(result, FALSE);
}

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

static gboolean mdx_resource_dir_has_files(const char *dir_path) {
    if (!dir_path || !g_file_test(dir_path, G_FILE_TEST_IS_DIR)) {
        return FALSE;
    }

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        return FALSE;
    }

    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.') {
            continue;
        }

        char *child = g_build_filename(dir_path, name, NULL);
        gboolean found = FALSE;

        if (g_file_test(child, G_FILE_TEST_IS_REGULAR)) {
            found = TRUE;
        } else if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
            found = mdx_resource_dir_has_files(child);
        }

        g_free(child);

        if (found) {
            g_dir_close(dir);
            return TRUE;
        }
    }

    g_dir_close(dir);
    return FALSE;
}

static char *mdx_resource_stamp_path(const char *resource_dir) {
    return g_build_filename(resource_dir, ".diction-mdd-stamp", NULL);
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

static char *mdx_build_resource_stamp(GPtrArray *mdd_paths) {
    if (!mdd_paths || mdd_paths->len == 0) {
        return NULL;
    }

    GString *stamp = g_string_new("format=3\n");
    for (guint i = 0; i < mdd_paths->len; i++) {
        const char *mdd_path = g_ptr_array_index(mdd_paths, i);
        struct stat st;
        if (stat(mdd_path, &st) != 0) {
            g_string_free(stamp, TRUE);
            return NULL;
        }

        g_string_append_printf(stamp,
                               "file=%s\nmtime=%lld\nsize=%lld\n",
                               mdd_path,
                               (long long)st.st_mtime,
                               (long long)st.st_size);
    }

    return g_string_free(stamp, FALSE);
}

static gboolean mdx_resource_stamp_matches(const char *resource_dir, GPtrArray *mdd_paths) {
    char *stamp_path = mdx_resource_stamp_path(resource_dir);
    char *expected = mdx_build_resource_stamp(mdd_paths);
    char *current = NULL;
    gsize current_len = 0;
    gboolean matches = FALSE;

    if (expected &&
        g_file_get_contents(stamp_path, &current, &current_len, NULL) &&
        strcmp(current, expected) == 0) {
        matches = TRUE;
    }

    g_free(current);
    g_free(expected);
    g_free(stamp_path);
    return matches;
}

static void mdx_write_resource_stamp(const char *resource_dir, GPtrArray *mdd_paths) {
    char *stamp_path = mdx_resource_stamp_path(resource_dir);
    char *contents = mdx_build_resource_stamp(mdd_paths);

    if (contents) {
        g_file_set_contents(stamp_path, contents, -1, NULL);
    }

    g_free(contents);
    g_free(stamp_path);
}

typedef struct { uint64_t off; char *name; } MDDRes;
static int mdd_res_cmp(const void *a, const void *b) {
    uint64_t oa = ((MDDRes*)a)->off, ob = ((MDDRes*)b)->off;
    return (oa < ob) ? -1 : (oa > ob) ? 1 : 0;
}

static gboolean mdx_extract_mdd_resources(const char *mdd_path, const char *dest_dir, int is_v2, int num_size, int encoding_is_utf16, int encrypted, volatile gint *cancel_flag, gint expected, const char *dict_path) {
    fprintf(stderr, "[MDD EXTRACT] Starting extraction from: %s\n", mdd_path);
    fprintf(stderr, "[MDD EXTRACT] Destination: %s\n", dest_dir);
    fprintf(stderr, "[MDD EXTRACT] Params: is_v2=%d, num_size=%d, utf16=%d, encrypted=%d\n", is_v2, num_size, encoding_is_utf16, encrypted);
    gboolean success = FALSE;

    FILE *f = fopen(mdd_path, "rb");
    if (!f) {
        fprintf(stderr, "[MDD EXTRACT] FAILED to open MDD file\n");
        return FALSE;
    }
    fprintf(stderr, "[MDD EXTRACT] MDD file opened successfully\n"); 
    unsigned char b4[4];
    if (fread(b4, 1, 4, f) != 4) {
        fprintf(stderr, "[MDD EXTRACT] FAILED to read header size\n");
        fclose(f);
        return FALSE;
    }
    uint32_t hts = ru32be(b4);
    fprintf(stderr, "[MDD EXTRACT] Header text size: %u\n", hts);

    int mdd_is_v2 = is_v2;
    int mdd_num_size = num_size;
    int mdd_encoding_is_utf16 = encoding_is_utf16;
    int mdd_encrypted = encrypted;

    if (hts <= 10 * 1024 * 1024) {
        unsigned char *header_raw = malloc(hts);
        if (header_raw && fread(header_raw, 1, hts, f) == hts) {
            size_t ascii_len = hts / 2;
            char *ascii_hdr = malloc(ascii_len + 1);
            if (ascii_hdr) {
                for (size_t i = 0; i < ascii_len; i++) {
                    ascii_hdr[i] = header_raw[i * 2];
                }
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
                        encoding_start++;
                        const char *encoding_end = strchr(encoding_start, '"');
                        if (encoding_end && encoding_end > encoding_start) {
                            size_t encoding_len = (size_t)(encoding_end - encoding_start);
                            char *encoding = g_strndup(encoding_start, encoding_len);
                            mdd_encoding_is_utf16 = (g_ascii_strcasecmp(encoding, "UTF-16") == 0);
                            g_free(encoding);
                        } else {
                            mdd_encoding_is_utf16 = 1;
                        }
                    }
                } else {
                    mdd_encoding_is_utf16 = 1;
                }

                char *xp = strstr(ascii_hdr, "Encrypted=\"");
                if (xp) {
                    mdd_encrypted = atoi(xp + 11);
                }

                fprintf(stderr, "[MDD EXTRACT] Header-derived params: is_v2=%d, num_size=%d, utf16=%d, encrypted=%d\n",
                        mdd_is_v2, mdd_num_size, mdd_encoding_is_utf16, mdd_encrypted);
                free(ascii_hdr);
            }
        }
        free(header_raw);
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) { fclose(f); return FALSE; }
    } else {
        fseek(f, hts, SEEK_CUR);
    }

    fseek(f, 4, SEEK_CUR);

    int kbh_size = mdd_is_v2 ? (mdd_num_size * 5) : (mdd_num_size * 4);
    fprintf(stderr, "[MDD EXTRACT] KBH size: %d (%s)\n", kbh_size, mdd_is_v2 ? "v2" : "v1");
    unsigned char *kbh = malloc(kbh_size);
    if (!kbh) {
        fprintf(stderr, "[MDD EXTRACT] FAILED to allocate KBH\n");
        fclose(f);
        return FALSE;
    }
    if (fread(kbh, 1, kbh_size, f) != (size_t)kbh_size) {
        fprintf(stderr, "[MDD EXTRACT] FAILED to read KBH\n");
        free(kbh);
        fclose(f);
        return FALSE;
    }
    const unsigned char *kp = kbh;
    uint64_t num_key_blocks = read_num(&kp, mdd_num_size);
    uint64_t num_entries = read_num(&kp, mdd_num_size);
    uint64_t kbi_decomp = mdd_is_v2 ? read_num(&kp, mdd_num_size) : 0;
    uint64_t kbi_comp = read_num(&kp, mdd_num_size);
    uint64_t kb_data_size = read_num(&kp, mdd_num_size);
    fprintf(stderr, "[MDD EXTRACT] num_key_blocks=%llu, num_entries=%llu, kbi_decomp=%llu, kbi_comp=%llu, kb_data_size=%llu\n",
            (unsigned long long)num_key_blocks, (unsigned long long)num_entries, (unsigned long long)kbi_decomp,
            (unsigned long long)kbi_comp, (unsigned long long)kb_data_size);
    free(kbh);
    if (mdd_is_v2) fseek(f, 4, SEEK_CUR);
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) { fclose(f); return FALSE; }
    unsigned char *kbi_raw = malloc(kbi_comp);
    if (!kbi_raw) {
        fprintf(stderr, "[MDD EXTRACT] FAILED to allocate KBI\n");
        fclose(f);
        return FALSE;
    }
    if (fread(kbi_raw, 1, kbi_comp, f) != kbi_comp) { free(kbi_raw); fclose(f); return FALSE; }
    long kb_data_pos = ftell(f);
    typedef struct { uint64_t comp, decomp; } KBI;
    KBI *kbis = calloc(num_key_blocks, sizeof(KBI));
    if (!kbis) { free(kbi_raw); fclose(f); return FALSE; }
    size_t kbc = 0;
    fprintf(stderr, "[MDD EXTRACT] Parsing KBIs (is_v2=%d, encrypted=%d)...\n", mdd_is_v2, mdd_encrypted);
    if (mdd_is_v2) {
        if (mdd_encrypted & 2) {
            fprintf(stderr, "[MDD EXTRACT] Decrypting KBI...\n");
            mdx_decrypt_key_block_info(kbi_raw, kbi_comp);
        }
        size_t dlen = 0;
        unsigned char *data = mdx_block_decompress(kbi_raw, kbi_comp, kbi_decomp, &dlen);
        if (data) {
            fprintf(stderr, "[MDD EXTRACT] KBI decompressed: %zu bytes\n", dlen);
            const unsigned char *ip = data, *ie = data + dlen;
            while (ip < ie && kbc < num_key_blocks) {
                if (ip + mdd_num_size > ie) {
                    fprintf(stderr, "[MDD EXTRACT] Not enough data for next KBI entry\n");
                    break;
                }
                ip += mdd_num_size;
                uint32_t head_size = read_u8or16(&ip, 1);
                fprintf(stderr, "[MDD EXTRACT] head_size=%u\n", head_size);
                ip += (mdd_encoding_is_utf16 ? (head_size+1)*2 : (head_size+1));
                uint32_t tail_size = read_u8or16(&ip, 1);
                fprintf(stderr, "[MDD EXTRACT] tail_size=%u\n", tail_size);
                ip += (mdd_encoding_is_utf16 ? (tail_size+1)*2 : (tail_size+1));
                if (ip + mdd_num_size * 2 > ie) {
                    fprintf(stderr, "[MDD EXTRACT] Not enough data for comp/decomp\n");
                    break;
                }
                kbis[kbc].comp = read_num(&ip, mdd_num_size);
                kbis[kbc].decomp = read_num(&ip, mdd_num_size);
                fprintf(stderr, "[MDD EXTRACT] KBI[%zu]: comp=%llu decomp=%llu\n", kbc, (unsigned long long)kbis[kbc].comp, (unsigned long long)kbis[kbc].decomp);
                kbc++;
            }
            fprintf(stderr, "[MDD EXTRACT] Parsed %zu KBIs from decompressed data\n", kbc);
            free(data);
        } else {
            fprintf(stderr, "[MDD EXTRACT] FAILED to decompress KBI\n");
        }
    } else {
        const unsigned char *ip = kbi_raw, *ie = kbi_raw + kbi_comp;
        while (ip < ie && kbc < num_key_blocks) {
            ip += mdd_num_size;
            uint32_t head_size = read_u8or16(&ip, 0); ip += (head_size+1);
            uint32_t tail_size = read_u8or16(&ip, 0); ip += (tail_size+1);
            if (ip + mdd_num_size * 2 > ie) break;
            kbis[kbc].comp = read_num(&ip, mdd_num_size);
            kbis[kbc].decomp = read_num(&ip, mdd_num_size);
            kbc++;
        }
    }
    free(kbi_raw);
    MDDRes *resources = calloc(num_entries, sizeof(MDDRes));
    if (!resources) { free(kbis); fclose(f); return FALSE; }
    size_t res_count = 0;
    fseek(f, kb_data_pos, SEEK_SET);
    for (size_t bi = 0; bi < kbc; bi++) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        fprintf(stderr, "[MDD EXTRACT] Processing key block %zu/%zu (comp=%llu, decomp=%llu)\n",
                bi, kbc, (unsigned long long)kbis[bi].comp, (unsigned long long)kbis[bi].decomp);
        unsigned char *comp = malloc(kbis[bi].comp);
        if (!comp) {
            fprintf(stderr, "[MDD EXTRACT] FAILED to allocate key block data (%llu bytes)\n", (unsigned long long)kbis[bi].comp);
            continue;
        }
        if (fread(comp, 1, kbis[bi].comp, f) != kbis[bi].comp) {
            fprintf(stderr, "[MDD EXTRACT] FAILED to read key block data\n");
            free(comp); continue;
        }
        size_t dlen = 0;
        fprintf(stderr, "[MDD EXTRACT] Decompressing key block...\n");
        unsigned char *data = mdx_block_decompress(comp, kbis[bi].comp, kbis[bi].decomp, &dlen);
        free(comp);
        if (!data) {
            fprintf(stderr, "[MDD EXTRACT] FAILED to decompress key block\n");
            continue;
        }
        fprintf(stderr, "[MDD EXTRACT] Decompressed %zu resources from key block\n", (size_t)((dlen / 64) + 1));
        const unsigned char *hp = data, *he = data + dlen;
        size_t kb_res_count = 0;
        while (hp < he && res_count < num_entries) {
            if (hp + mdd_num_size > he) break;
            resources[res_count].off = (mdd_num_size == 8) ? ru64be(hp) : ru32be(hp);
            hp += mdd_num_size;
            char word[1024];
            if (mdd_encoding_is_utf16) {
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
            kb_res_count++;
        }
        fprintf(stderr, "[MDD EXTRACT] Extracted %zu resources from this key block, total res_count=%zu\n", kb_res_count, res_count);
        free(data);
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
    }
    fprintf(stderr, "[MDD EXTRACT] Sorting %zu resources\n", res_count);
    qsort(resources, res_count, sizeof(MDDRes), mdd_res_cmp);
    fprintf(stderr, "[MDD EXTRACT] Reading resource blocks...\n");
    fseek(f, kb_data_pos + kb_data_size, SEEK_SET);
    unsigned char rbh[64]; if (fread(rbh, 1, mdd_num_size * 4, f) != (size_t)(mdd_num_size * 4)) {}
    const unsigned char *rp = rbh;
    uint64_t nrb = read_num(&rp, mdd_num_size);
    read_num(&rp, mdd_num_size);
    read_num(&rp, mdd_num_size);
    typedef struct { uint64_t comp, decomp; } RBI;
    RBI *rbis = calloc(nrb, sizeof(RBI));
    if (rbis) {
        for (uint64_t i = 0; i < nrb; i++) {
            unsigned char p[16]; if(fread(p, 1, mdd_num_size * 2, f) != (size_t)(mdd_num_size * 2)) break;
            const unsigned char *pp = p;
            rbis[i].comp = read_num(&pp, mdd_num_size);
            rbis[i].decomp = read_num(&pp, mdd_num_size);
        }
        uint64_t td = 0;
            for (uint64_t i = 0; i < nrb; i++) td += rbis[i].decomp;
        unsigned char *all_recs = malloc(td);
        if (all_recs) {
            uint64_t co = 0;
            for (uint64_t i = 0; i < nrb; i++) {
                    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
                unsigned char *comp = malloc(rbis[i].comp);
                if (!comp) continue;
                if (fread(comp, 1, rbis[i].comp, f) == rbis[i].comp) {
                    size_t dlen = 0;
                    unsigned char *data = mdx_block_decompress(comp, rbis[i].comp, rbis[i].decomp, &dlen);
                    if (data) { memcpy(all_recs + co, data, dlen); co += dlen; free(data); }
                }
                free(comp);
            }
            fprintf(stderr, "[MDD EXTRACT] Starting to write %zu resources\n", res_count);
            size_t written_count = 0;
            int last_pct = -1;
            for (size_t i = 0; i < res_count; i++) {
                if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
                if (!resources[i].name[0]) {
                    fprintf(stderr, "[MDD EXTRACT] Skipping empty resource %zu\n", i);
                    continue;
                }
                uint64_t start = resources[i].off;
                uint64_t end = (i + 1 < res_count) ? resources[i+1].off : td;
                fprintf(stderr, "[MDD EXTRACT] Resource %zu: '%s' (offset %llu-%llu, len %llu)\n", 
                        i, resources[i].name, (unsigned long long)start, (unsigned long long)end, 
                        (unsigned long long)(end - start));
                if (start < td && end <= td && end > start) {
                    char *full = g_build_filename(dest_dir, resources[i].name, NULL);
                    char *parent = g_path_get_dirname(full);
                    fprintf(stderr, "[MDD EXTRACT] Making parent dir: %s\n", parent);
                    g_mkdir_with_parents(parent, 0755);
                    fprintf(stderr, "[MDD EXTRACT] Opening file: %s\n", full);
                    FILE *rf = fopen(full, "wb");
                    if (rf) {
                        fprintf(stderr, "[MDD EXTRACT] Writing %llu bytes\n", (unsigned long long)(end - start));
                        fwrite(all_recs + start, 1, (size_t)(end - start), rf);
                        fclose(rf);
                        fprintf(stderr, "[MDD EXTRACT] Successfully wrote: %s\n", full);
                        written_count++;
                        if (dict_path) {
                            int pct = (int)((i * 100) / (res_count ? res_count : 1));
                            if (pct != last_pct) {
                                last_pct = pct;
                                extern void settings_scan_progress_notify(const char *path, int percent);
                                settings_scan_progress_notify(dict_path, pct);
                            }
                        }
                    } else {
                        fprintf(stderr, "[MDD EXTRACT] FAILED to open: %s (errno: %d)\n", full, errno);
                    }
                    g_free(full); g_free(parent);
                } else {
                    fprintf(stderr, "[MDD EXTRACT] Invalid offsets: start=%llu td=%llu end=%llu\n", 
                            (unsigned long long)start, (unsigned long long)td, (unsigned long long)end);
                }
            }
            success = (written_count > 0);
            free(all_recs);
        }
        free(rbis);
    }
    for(size_t i=0; i<res_count; i++) free(resources[i].name);
    free(resources); free(kbis); fclose(f);
    return success;
}

static char *mdx_prepare_resource_dir(const char *path, int is_v2, int num_size, int encoding_is_utf16, int encrypted, volatile gint *cancel_flag, gint expected) {
    char *mdx_dir = g_path_get_dirname(path);
    char *mdx_basename = g_path_get_basename(path);
    char *dot_pos = strrchr(mdx_basename, '.');
    if (dot_pos) *dot_pos = '\0';

    const char *cache_base = get_cache_base_dir();
    char *resource_dir = g_build_filename(cache_base, "diction", "resources", mdx_basename, NULL);

    fprintf(stderr, "[MDX RESOURCES] Resource dir: %s\n", resource_dir);
    fprintf(stderr, "[MDX RESOURCES] Dictionary dir: %s\n", mdx_dir);
    fprintf(stderr, "[MDX RESOURCES] Dictionary basename: %s\n", mdx_basename);

    GPtrArray *mdd_paths = mdx_collect_mdd_paths(path);
    gboolean has_files = mdx_resource_dir_has_files(resource_dir);
    gboolean needs_extract = FALSE;

    if (mdd_paths && mdd_paths->len > 0) {
        if (!has_files) {
            needs_extract = TRUE;
            if (g_file_test(resource_dir, G_FILE_TEST_IS_DIR)) {
                fprintf(stderr, "[MDX RESOURCES] Resource directory exists but is empty or incomplete, retrying extraction\n");
            }
        } else if (!mdx_resource_stamp_matches(resource_dir, mdd_paths)) {
            needs_extract = TRUE;
            fprintf(stderr, "[MDX RESOURCES] Resource directory is stale or from an older extractor, retrying extraction\n");
        }
    }

        if (needs_extract) {
        fprintf(stderr, "[MDX RESOURCES] Found %u MDD file(s); extracting to: %s\n", mdd_paths ? mdd_paths->len : 0, resource_dir);
        g_mkdir_with_parents(resource_dir, 0755);
        gboolean extracted_any = FALSE;
        for (guint i = 0; mdd_paths && i < mdd_paths->len; i++) {
            const char *mdd_path = g_ptr_array_index(mdd_paths, i);
            fprintf(stderr, "[MDX RESOURCES] Extracting MDD %u/%u: %s\n", i + 1, mdd_paths->len, mdd_path);
                if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                    /* Abort extraction */
                    break;
                }
                extracted_any = mdx_extract_mdd_resources(mdd_path, resource_dir, is_v2, num_size, encoding_is_utf16, encrypted, cancel_flag, expected, path) || extracted_any;
        }
        if (extracted_any) {
            mdx_write_resource_stamp(resource_dir, mdd_paths);
            fprintf(stderr, "[MDX RESOURCES] Extraction complete\n");
        } else {
            fprintf(stderr, "[MDX RESOURCES] Extraction failed or produced no files\n");
        }
    } else if (has_files) {
        fprintf(stderr, "[MDX RESOURCES] Resource directory already populated\n");
    } else {
        if (mdd_paths && mdd_paths->len > 0) {
            for (guint i = 0; i < mdd_paths->len; i++) {
                fprintf(stderr, "[MDX RESOURCES] Found MDD: %s\n", (char *)g_ptr_array_index(mdd_paths, i));
            }
        }
        fprintf(stderr, "[MDX RESOURCES] MDD not found\n");
    }

    if (mdd_paths) {
        g_ptr_array_free(mdd_paths, TRUE);
    }
    g_free(mdx_dir);
    g_free(mdx_basename);

    if (mdx_resource_dir_has_files(resource_dir)) {
        return resource_dir;
    }

    g_free(resource_dir);
    return NULL;
}


DictMmap *parse_mdx_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    ensure_cache_directory();

    char *cache_path = get_cached_dict_path(path);
    gboolean cache_valid = (access(cache_path, F_OK) == 0) &&
                           is_cache_valid(cache_path, path);

    int cache_fd = -1;
    const char *dict_data = NULL;
    size_t dict_size = 0;
    char *title = NULL;
    char *stylesheet = NULL;
    char *source_dir = g_path_get_dirname(path);

    /* ── read header ── */
    FILE *fh = fopen(path, "rb");
    int is_v2 = 0, num_size = 4, encoding_is_utf16 = 0, encrypted = 0;
    uint32_t header_text_size = 0;

    if (fh) {
        unsigned char buf4[4];
        if (fread(buf4, 1, 4, fh) == 4) {
            header_text_size = ru32be(buf4);
            if (header_text_size <= 10*1024*1024) {
                unsigned char *header_raw = malloc(header_text_size);
                fread(header_raw, 1, header_text_size, fh);

                size_t ascii_len = header_text_size / 2;
                char *ascii_hdr = malloc(ascii_len + 1);
                for (size_t i = 0; i < ascii_len; i++)
                    ascii_hdr[i] = header_raw[i * 2];
                ascii_hdr[ascii_len] = '\0';

                const char *tp = strstr(ascii_hdr, "Title=\"");
                if (tp) {
                    const char *ts = tp + 7;
                    const char *te = strchr(ts, '"');
                    if (te && (te - ts) > 0) {
                        char *raw_title = strndup(ts, te - ts);
                        char *validated = validate_utf8_string(raw_title);
                        char *stripped = strip_html_tags(validated);
                        /* Skip placeholder/invalid titles */
                        if (strcmp(stripped, "Title (No HTML code allowed)") != 0 && 
                            strlen(stripped) > 0) {
                            title = stripped;
                            fprintf(stderr, "[MDX DEBUG] Title: '%s'\n", title);
                        } else {
                            g_free(stripped);
                            fprintf(stderr, "[MDX DEBUG] Skipped placeholder title\n");
                        }
                        g_free(validated);
                        free(raw_title);
                    }
                }

                char *vp = strstr(ascii_hdr, "GeneratedByEngineVersion=\"");
                if (vp) {
                    const char *vptr = strchr(vp, '\"');
                    if (vptr) {
                        double ver = atof(vptr + 1);
                        is_v2 = (ver >= 2.0);
                        num_size = is_v2 ? 8 : 4;
                    }
                }

                char *ep = strstr(ascii_hdr, "Encoding=\"");
                if (ep && (strstr(ep, "UTF-16") || strstr(ep, "utf-16"))) encoding_is_utf16 = 1;

                char *xp = strstr(ascii_hdr, "Encrypted=\"");
                if (xp) encrypted = atoi(xp + 11);

                stylesheet = extract_header_attribute(ascii_hdr, "StyleSheet");

                free(ascii_hdr);
                free(header_raw);
            }
        }
    }

    /* ───────────────────────────── */
    /* FAST PATH: use cache directly */
    /* ───────────────────────────── */
    if (cache_valid) {
        if (fh) fclose(fh);

        cache_fd = open(cache_path, O_RDONLY);
        struct stat st;
        if (fstat(cache_fd, &st) != 0 || st.st_size < 16) {
            close(cache_fd);
            g_free(cache_path);
            return NULL;
        }

        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);

        DictMmap *dict = calloc(1, sizeof(DictMmap));
        dict->fd = cache_fd;
        dict->data = dict_data;
        dict->size = dict_size;
        dict->name = title;
        dict->source_dir = g_strdup(source_dir);
        dict->mdx_stylesheet = stylesheet ? g_strdup(stylesheet) : NULL;
        dict->index = splay_tree_new(dict->data, dict->size);

        /* FAST LOADING: read index array from end of file */
        uint64_t count = *(uint64_t*)dict->data;
        size_t index_size = (size_t)count * sizeof(TreeEntry);
        if (count > 0 && index_size + 8 <= dict->size) {
            size_t index_off = dict->size - index_size;
            TreeEntry *tree_entries = (TreeEntry*)(dict->data + index_off);
            /* Validate entries to avoid using corrupt or stale indexes */
            size_t data_region_end = dict->size - index_size;
            gboolean valid_index = TRUE;
            for (uint64_t i = 0; i < count; i++) {
                int64_t h_off = tree_entries[i].h_off;
                uint64_t h_len = tree_entries[i].h_len;
                int64_t d_off = tree_entries[i].d_off;
                uint64_t d_len = tree_entries[i].d_len;
                if (h_off < 8 || (uint64_t)h_off >= data_region_end) { valid_index = FALSE; break; }
                if (d_off < 8 || (uint64_t)d_off >= data_region_end) { valid_index = FALSE; break; }
                if (h_len == 0 || d_len == 0) { valid_index = FALSE; break; }
                if ((uint64_t)h_off + h_len > data_region_end) { valid_index = FALSE; break; }
                if ((uint64_t)d_off + d_len > data_region_end) { valid_index = FALSE; break; }
            }
            if (valid_index) {
                insert_balanced(dict->index, tree_entries, 0, (int)count - 1);
            } else {
                fprintf(stderr, "[MDX] Cache index validation failed for %s — rebuilding index.\n", path);
            }
        }

        if (!(cancel_flag && g_atomic_int_get(cancel_flag) != expected))
            dict->resource_dir = mdx_prepare_resource_dir(path, is_v2, num_size, encoding_is_utf16, encrypted, cancel_flag, expected);

        g_free(stylesheet);
        g_free(source_dir);
        g_free(cache_path);
        return dict;
    }

    /* ───────────────────────────── */
    /* BUILD CACHE (ZERO-COPY)       */
    /* ───────────────────────────── */

    FILE *cache = fopen(cache_path, "wb");
    if (!cache) {
        if (fh) fclose(fh);
        g_free(cache_path);
        return NULL;
    }

    uint64_t zero_count = 0;
    fwrite(&zero_count, 8, 1, cache); 

    fseek(fh, 4 + header_text_size + 4, SEEK_SET); // skip HeaderLen + Header + Checksum

    int kbh_size = is_v2 ? (num_size * 5) : (num_size * 4);
    unsigned char *kbh = malloc(kbh_size);
    fread(kbh, 1, kbh_size, fh);

    const unsigned char *kp = kbh;
    uint64_t num_key_blocks = read_num(&kp, num_size); // Read but not used in this context
    (void)num_key_blocks; // Suppress unused variable warning
    uint64_t num_entries = read_num(&kp, num_size);
    uint64_t kbi_decomp = is_v2 ? read_num(&kp, num_size) : 0;
    uint64_t kbi_comp = read_num(&kp, num_size);
    uint64_t kb_data_size = read_num(&kp, num_size);
    free(kbh);

    if (is_v2) fseek(fh, 4, SEEK_CUR);
    unsigned char *kbi_raw = malloc(kbi_comp);
    fread(kbi_raw, 1, kbi_comp, fh);

    TreeEntry *tree_entries = calloc(num_entries, sizeof(TreeEntry));
    size_t valid_count = 0;

    if (is_v2 && (encrypted & 2)) mdx_decrypt_key_block_info(kbi_raw, kbi_comp);
    size_t kbi_dlen = 0;
    unsigned char *kbi_data = mdx_block_decompress(kbi_raw, kbi_comp, kbi_decomp, &kbi_dlen);
    free(kbi_raw);

    if (kbi_data) {
        const unsigned char *ip = kbi_data, *ie = kbi_data + kbi_dlen;
        while (ip < ie && valid_count < num_entries) {
            ip += num_size;
            uint32_t head_size = read_u8or16(&ip, is_v2); ip += (encoding_is_utf16 ? (head_size+1)*2 : (head_size+1));
            uint32_t tail_size = read_u8or16(&ip, is_v2); ip += (encoding_is_utf16 ? (tail_size+1)*2 : (tail_size+1));
            uint64_t comp_size = read_num(&ip, num_size);
            uint64_t decomp_size = read_num(&ip, num_size);

            long next_kb = ftell(fh) + comp_size;
            unsigned char *kb_comp = malloc(comp_size);
            if (fread(kb_comp, 1, comp_size, fh) == comp_size) {
                size_t kb_dlen = 0;
                unsigned char *kb_data = mdx_block_decompress(kb_comp, comp_size, decomp_size, &kb_dlen);
                if (kb_data) {
                    const unsigned char *kp_ent = kb_data, *ke_ent = kb_data + kb_dlen;
                    while (kp_ent < ke_ent && valid_count < num_entries) {
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
                            memcpy(word, ws, wl); word[wl] = '\0';
                            if (kp_ent < ke_ent) kp_ent++;
                        }
                        size_t wlen = strlen(word);
                        long hw_off = ftell(cache);
                        fwrite(word, 1, wlen, cache);
                        fwrite("\n", 1, 1, cache);

                        tree_entries[valid_count].h_off = (int64_t)hw_off;
                        tree_entries[valid_count].h_len = (uint64_t)wlen;
                        tree_entries[valid_count].d_off = (int64_t)id;
                        valid_count++;
                    }
                    free(kb_data);
                }
            }
            free(kb_comp);
            fseek(fh, next_kb, SEEK_SET);
        }
        free(kbi_data);
    }

    fseek(fh, 4 + header_text_size + 4 + kbh_size + (is_v2?4:0) + kbi_comp + kb_data_size, SEEK_SET);

    unsigned char rbh[64];
    fread(rbh, 1, num_size * 4, fh);
    const unsigned char *rp = rbh;
    uint64_t nrb = read_num(&rp, num_size);
    read_num(&rp, num_size);
    read_num(&rp, num_size);

    typedef struct { uint64_t comp, decomp; } RB;
    RB *rbs = calloc(nrb, sizeof(RB));
    uint64_t total_decomp = 0;
    for (uint64_t i = 0; i < nrb; i++) {
        unsigned char tmp[16];
        fread(tmp, 1, num_size * 2, fh);
        const unsigned char *ppb = tmp;
        rbs[i].comp = read_num(&ppb, num_size);
        rbs[i].decomp = read_num(&ppb, num_size);
        total_decomp += rbs[i].decomp;
    }

    unsigned char *dict_rec_data = malloc(total_decomp);
    uint64_t co = 0;
    for (uint64_t i = 0; i < nrb; i++) {
        unsigned char *comp = malloc(rbs[i].comp);
        if (fread(comp, 1, rbs[i].comp, fh) == rbs[i].comp) {
            size_t dlen = 0;
            unsigned char *data = mdx_block_decompress(comp, rbs[i].comp, rbs[i].decomp, &dlen);
            if (data) { memcpy(dict_rec_data + co, data, dlen); co += dlen; free(data); }
        }
        free(comp);
    }
    free(rbs);

    for (size_t i = 0; i < valid_count; i++) {
        uint64_t start = (uint64_t)tree_entries[i].d_off;
        uint64_t end = (i + 1 < valid_count) ? (uint64_t)tree_entries[i+1].d_off : total_decomp;

        if (end > start && end <= total_decomp) {
            long def_off = ftell(cache);
            fwrite(dict_rec_data + start, 1, (size_t)(end - start), cache);
            fwrite("\n", 1, 1, cache);
            tree_entries[i].d_off = (int64_t)def_off;
            tree_entries[i].d_len = (uint64_t)(end - start);
        } else {
            tree_entries[i].d_len = 0;
        }
    }
    free(dict_rec_data);

    /* Write index array at end of file */
    fwrite(tree_entries, sizeof(TreeEntry), valid_count, cache);

    fseek(cache, 0, SEEK_SET);
    uint64_t final_cnt = (uint64_t)valid_count;
    fwrite(&final_cnt, 8, 1, cache);
    
    fclose(cache);
    fclose(fh);

    cache_fd = open(cache_path, O_RDONLY);
    struct stat st_final;
    fstat(cache_fd, &st_final);
    dict_size = st_final.st_size;
    dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);

    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = cache_fd;
    dict->data = dict_data;
    dict->size = dict_size;
    dict->name = title;
    dict->source_dir = source_dir;
    dict->mdx_stylesheet = stylesheet;
    dict->index = splay_tree_new(dict->data, dict->size);

    insert_balanced(dict->index, tree_entries, 0, (int)valid_count - 1);
    free(tree_entries);
    g_free(cache_path);

    dict->resource_dir = mdx_prepare_resource_dir(path, is_v2, num_size, encoding_is_utf16, encrypted, cancel_flag, expected);

    return dict;
}
