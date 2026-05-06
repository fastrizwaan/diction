#include "dictzip.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define DZ_MAX_BLOCKS 65536

struct DictZip {
    FILE *f;
    uint32_t chlen;  /* Block size */
    uint32_t chcnt;  /* Number of blocks */
    uint32_t *lens;  /* Compressed lengths */
    uint64_t *offs;  /* Compressed offsets */
    uint64_t first_block_off;
};

static uint16_t ru16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t ru32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

DictZip* dictzip_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    unsigned char hdr[10];
    if (fread(hdr, 1, 10, f) != 10) { fclose(f); return NULL; }

    /* Check gzip signature */
    if (hdr[0] != 0x1F || hdr[1] != 0x8B || hdr[2] != 8) { fclose(f); return NULL; }

    uint8_t flags = hdr[3];
    if (!(flags & 0x04)) { fclose(f); return NULL; } /* Must have FEXTRA */

    uint16_t xlen = 0;
    if (fread(&xlen, 2, 1, f) != 1) { fclose(f); return NULL; }

    unsigned char *extra = malloc(xlen);
    if (fread(extra, 1, xlen, f) != xlen) { free(extra); fclose(f); return NULL; }

    unsigned char *p = extra;
    unsigned char *end = extra + xlen;
    DictZip *dz = NULL;

    while (p + 4 <= end) {
        uint16_t sublen = ru16(p + 2);
        if (p + 4 + sublen > end) break;

        if (p[0] == 'R' && p[1] == 'A') {
            /* DictZip subfield: VER(2), CHLEN(2), CHCNT(2), LENS(2*CHCNT) */
            if (sublen < 6) break;
            
            dz = calloc(1, sizeof(DictZip));
            dz->f = f;
            dz->chlen = ru16(p + 4 + 2);
            dz->chcnt = ru16(p + 4 + 4);
            
            if (sublen < 6 + 2 * dz->chcnt) { free(dz); dz = NULL; break; }
            
            dz->lens = malloc(dz->chcnt * sizeof(uint32_t));
            dz->offs = malloc(dz->chcnt * sizeof(uint64_t));
            
            for (uint32_t i = 0; i < dz->chcnt; i++) {
                dz->lens[i] = ru16(p + 4 + 6 + 2 * i);
            }
            
            uint64_t off = dz->first_block_off;
            for (uint32_t i = 0; i < dz->chcnt; i++) {
                dz->offs[i] = off;
                off += dz->lens[i];
            }
            break;
        }
        p += 4 + sublen;
    }
    free(extra);

    if (!dz) { fclose(f); return NULL; }

    /* Skip other fields to find first block offset */
    if (flags & 0x08) { /* FNAME */
        while (fgetc(f) > 0);
    }
    if (flags & 0x10) { /* FCOMMENT */
        while (fgetc(f) > 0);
    }
    if (flags & 0x02) { /* FHCRC */
        fseek(f, 2, SEEK_CUR);
    }

    dz->first_block_off = ftell(f);
    
    uint64_t off = dz->first_block_off;
    for (uint32_t i = 0; i < dz->chcnt; i++) {
        dz->offs[i] = off;
        off += dz->lens[i];
    }

    return dz;
}

unsigned char* dictzip_read(DictZip *dz, uint64_t offset, uint32_t length, size_t *out_len) {
    if (length == 0) {
        unsigned char *result = malloc(2);
        result[0] = result[1] = '\0';
        *out_len = 0;
        return result;
    }
    uint32_t start_block = offset / dz->chlen;
    uint32_t end_block = (offset + length - 1) / dz->chlen;
    
    if (end_block >= dz->chcnt) {
        printf("[DZ ERROR] end_block %u >= chcnt %u\n", end_block, dz->chcnt);
        return NULL;
    }

    size_t total_buf_size = (size_t)(end_block - start_block + 1) * dz->chlen;
    unsigned char *full_decomp = malloc(total_buf_size);
    if (!full_decomp) {
        printf("[DZ ERROR] Could not allocate full_decomp (%zu bytes)\n", total_buf_size);
        return NULL;
    }
    size_t decomp_ptr = 0;

    for (uint32_t i = start_block; i <= end_block; i++) {
        if (fseek(dz->f, dz->offs[i], SEEK_SET) != 0) {
            printf("[DZ ERROR] fseek failed for block %u at offset %lu\n", i, (unsigned long)dz->offs[i]);
            free(full_decomp); return NULL;
        }
        unsigned char *comp = malloc(dz->lens[i]);
        if (!comp) {
            printf("[DZ ERROR] Could not allocate comp buffer (%u bytes)\n", dz->lens[i]);
            free(full_decomp); return NULL;
        }
        if (fread(comp, 1, dz->lens[i], dz->f) != dz->lens[i]) {
            printf("[DZ ERROR] fread failed for block %u\n", i);
            free(comp); free(full_decomp); return NULL;
        }

        z_stream strm = {0};
        strm.next_in = comp;
        strm.avail_in = dz->lens[i];
        strm.next_out = full_decomp + decomp_ptr;
        strm.avail_out = dz->chlen;

        if (inflateInit2(&strm, -15) != Z_OK) {
            printf("[DZ ERROR] inflateInit2 failed\n");
            free(comp); free(full_decomp); return NULL;
        }
        int ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        free(comp);

        if (ret != Z_STREAM_END && ret != Z_OK) {
            printf("[DZ ERROR] inflate failed with ret %d for block %u\n", ret, i);
            free(full_decomp); return NULL;
        }
        decomp_ptr += (dz->chlen - strm.avail_out);
    }

    uint32_t in_block_off = (uint32_t)(offset % dz->chlen);
    unsigned char *result = malloc(length + 2);
    if (!result) {
        printf("[DZ ERROR] Could not allocate result (%u bytes)\n", length + 2);
        free(full_decomp); return NULL;
    }
    memcpy(result, full_decomp + in_block_off, length);
    result[length] = '\0';
    result[length + 1] = '\0';
    *out_len = length;

    free(full_decomp);
    return result;
}

void dictzip_close(DictZip *dz) {
    if (!dz) return;
    if (dz->f) fclose(dz->f);
    free(dz->lens);
    free(dz->offs);
    free(dz);
}

uint64_t dictzip_get_uncompressed_size(DictZip *dz) {
    if (!dz || !dz->f) return 0;
    long current = ftell(dz->f);
    fseek(dz->f, -4, SEEK_END);
    unsigned char buf[4];
    if (fread(buf, 1, 4, dz->f) != 4) {
        fseek(dz->f, current, SEEK_SET);
        return 0;
    }
    fseek(dz->f, current, SEEK_SET);
    return ru32(buf);
}
