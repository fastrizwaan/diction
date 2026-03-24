/* ripemd128.h — Pure C RIPEMD-128 implementation
 * Based on goldendict-ng/src/dict/utils/ripemd.cc (libavutil)
 * Used for MDX key block info decryption */

#ifndef RIPEMD128_H
#define RIPEMD128_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t count;
    uint8_t  buffer[64];
    uint32_t state[10];
} RIPEMD128_CTX;

void ripemd128_init(RIPEMD128_CTX *ctx);
void ripemd128_update(RIPEMD128_CTX *ctx, const uint8_t *data, size_t len);
void ripemd128_digest(RIPEMD128_CTX *ctx, uint8_t digest[16]);

#endif
