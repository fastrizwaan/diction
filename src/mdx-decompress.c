#include "mdx-decompress.h"
#include <zlib.h>
#include <lzo/lzo1x.h>
#include <string.h>
#include "ripemd128.h"

static uint32_t ru32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

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
    if (lzo_ready != 1) return NULL;

    size_t cap = hint ? hint : (src_len * 8 + 4096);
    if (cap == 0) cap = 4096;

    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned char *dst = g_malloc(cap);
        if (!dst) return NULL;

        lzo_uint actual = (lzo_uint)cap;
        int ret = lzo1x_decompress_safe(src, (lzo_uint)src_len, dst, &actual, NULL);
        if (ret == LZO_E_OK) {
            *out_len = (size_t)actual;
            return dst;
        }

        g_free(dst);
        if (ret != LZO_E_OUTPUT_OVERRUN) return NULL;
        cap *= 2;
    }
    return NULL;
}

unsigned char *mdx_block_decompress(const unsigned char *block,
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
    if (type == 0x01000000) return lzo_inflate(payload, payload_len, decomp_hint, out_len);
    if (type == 0x02000000) return zlib_inflate(payload, payload_len, decomp_hint, out_len);
    return NULL;
}

void mdx_decrypt_key_block_info(unsigned char *buf, size_t len) {
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
