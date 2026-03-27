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
#include <utime.h>
#include <glib.h>

/* Cache helpers */
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

/* Read a big-endian integer of 1-4 bytes from a buffer */
static unsigned int read_be(const unsigned char *p, int bytes) {
    unsigned int val = 0;
    for (int i = 0; i < bytes; i++)
        val = (val << 8) | p[i];
    return val;
}

static char *find_bgl_resource_dir(const char *path) {
    char *base = g_strdup(path);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = '\0';
    }

    char *resource_dir = g_strconcat(base, ".files", NULL);
    g_free(base);

    if (g_file_test(resource_dir, G_FILE_TEST_IS_DIR)) {
        return resource_dir;
    }

    g_free(resource_dir);
    return NULL;
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
        printf("[BGL] Loading from cache: %s\n", cache_path);
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
    } else {
        // Need to extract and convert
        printf("[BGL] Building cache from source: %s\n", path);

        // Open cache file for writing
        FILE *cache_file = fopen(cache_path, "wb");
        if (!cache_file) {
            gzclose(gz);
            g_free(cache_path);
            return NULL;
        }

        unsigned char buf[65536];
        int n;
        while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
            fwrite(buf, 1, n, cache_file);
        }
        gzclose(gz);
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
    }

    g_free(cache_path);

    DictMmap *dict = calloc(1, sizeof(DictMmap));
    dict->fd = cache_fd;
    dict->tmp_file = NULL;
    dict->data = dict_data;
    dict->size = dict_size;
    dict->resource_dir = find_bgl_resource_dir(path);
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
        } else if (block_type == 3) {
            /* Info block: sub-type 1 is usually the title */
            if (block_len > 1) {
                unsigned int sub_type = block_data[0];
                if (sub_type == 1 && !dict->name) {
                    dict->name = strndup((const char *)block_data + 1, block_len - 1);
                }
            }
        }
    }

    printf("[BGL] Parsed %d entries from %s\n", word_count, path);
    return dict;
}
