/* dict-bgl.c — Babylon BGL dictionary parser
 *
 * BGL files begin with a 6-byte header:
 *   bytes 0-3: signature 0x12340001 or 0x12340002
 *   bytes 4-5: offset to gzip stream (big-endian u16)
 *
 * The gzip stream contains typed blocks:
 *   - Type 0: metadata (charset, etc.)
 *   - Type 1,7,10,11: dictionary entries
 *   - Type 3: info blocks (title, author, etc.)
 *   - Type 4: end-of-file
 *
 * We decompress the entire gzip payload into a tmpfile, then parse
 * the block stream to extract headword→definition pairs into the
 * SplayTree using byte offsets into the decompressed data.
 *
 * Reference: goldendict-ng/src/dict/bgl_babylon.cc
 */

#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

/* Read a big-endian integer of 1-4 bytes from a buffer */
static unsigned int read_be(const unsigned char *p, int bytes) {
    unsigned int val = 0;
    for (int i = 0; i < bytes; i++)
        val = (val << 8) | p[i];
    return val;
}

DictMmap* parse_bgl_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    unsigned char hdr[6];
    if (fread(hdr, 1, 6, f) < 6) { fclose(f); return NULL; }

    /* Verify BGL signature: 0x12 0x34 0x00 0x01|0x02 */
    if (hdr[0] != 0x12 || hdr[1] != 0x34 || hdr[2] != 0x00 ||
        (hdr[3] != 0x01 && hdr[3] != 0x02)) {
        fprintf(stderr, "[BGL] Invalid signature in %s\n", path);
        fclose(f);
        return NULL;
    }

    /* Offset to gzip data */
    int gz_offset = (hdr[4] << 8) | hdr[5];
    if (gz_offset < 6) { fclose(f); return NULL; }

    /* Seek to gz_offset and use gzdopen to read the gzip stream */
    fseek(f, gz_offset, SEEK_SET);
    int fd_dup = dup(fileno(f));
    lseek(fd_dup, gz_offset, SEEK_SET);
    fclose(f);

    gzFile gz = gzdopen(fd_dup, "rb");
    if (!gz) { close(fd_dup); return NULL; }

    /* Decompress entire stream into a tmpfile for mmap */
    FILE *tmp = tmpfile();
    if (!tmp) { gzclose(gz); return NULL; }

    unsigned char buf[65536];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, tmp);
    }
    gzclose(gz);
    fflush(tmp);

    /* mmap the decompressed data */
    int tmp_fd = fileno(tmp);
    struct stat st;
    if (fstat(tmp_fd, &st) < 0 || st.st_size == 0) {
        fclose(tmp);
        return NULL;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, tmp_fd, 0);
    if (map == MAP_FAILED) {
        fclose(tmp);
        return NULL;
    }

    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = tmp_fd;
    dict->tmp_file = tmp;
    dict->data = (const char *)map;
    dict->size = st.st_size;
    dict->index = splay_tree_new(dict->data, dict->size);

    /* Parse the block stream */
    const unsigned char *p = (const unsigned char *)dict->data;
    const unsigned char *end = p + dict->size;
    int word_count = 0;

    while (p < end) {
        if (p + 1 > end) break;

        unsigned int first_byte = p[0];
        unsigned int block_type = first_byte & 0x0F;

        if (block_type == 4) break; /* End of file marker */

        unsigned int len_code = first_byte >> 4;
        p++;

        unsigned int block_len;
        if (len_code < 4) {
            int num_bytes = len_code + 1;
            if (p + num_bytes > end) break;
            block_len = read_be(p, num_bytes);
            p += num_bytes;
        } else {
            block_len = len_code - 4;
        }

        if (block_len == 0 || p + block_len > end) {
            p += block_len;
            continue;
        }

        const unsigned char *block_data = p;
        p += block_len;

        /* Process entry blocks (type 1, 7, 10, 11) */
        if (block_type == 1 || block_type == 7 ||
            block_type == 10 || block_type == 11) {

            unsigned int pos = 0;
            unsigned int hw_len;

            if (block_type == 11) {
                if (pos + 5 > block_len) continue;
                pos = 1;
                hw_len = read_be(block_data + pos, 4);
                pos += 4;
            } else {
                if (pos + 1 > block_len) continue;
                hw_len = block_data[pos++];
            }

            if (pos + hw_len > block_len) continue;

            /* Headword offset/length (relative to dict->data) */
            size_t hw_offset = (const char *)(block_data + pos) - dict->data;
            pos += hw_len;

            /* Definition: skip to definition length */
            unsigned int def_len;
            if (block_type == 11) {
                /* Skip alternate forms count + data */
                if (pos + 4 > block_len) continue;
                unsigned int alts_num = read_be(block_data + pos, 4);
                pos += 4;
                for (unsigned int j = 0; j < alts_num; j++) {
                    if (pos + 4 > block_len) break;
                    unsigned int alt_len = read_be(block_data + pos, 4);
                    pos += 4 + alt_len;
                }
                if (pos + 4 > block_len) continue;
                def_len = read_be(block_data + pos, 4);
                pos += 4;
            } else {
                if (pos + 2 > block_len) continue;
                def_len = read_be(block_data + pos, 2);
                pos += 2;
            }

            if (pos + def_len > block_len) {
                def_len = block_len - pos;
            }

            size_t def_offset = (const char *)(block_data + pos) - dict->data;

            if (hw_len > 0 && def_len > 0) {
                splay_tree_insert(dict->index, hw_offset, hw_len,
                                  def_offset, def_len);
                word_count++;
            }
        }
    }

    printf("[BGL] Parsed %d entries from %s\n", word_count, path);
    return dict;
}
