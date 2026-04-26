/* dict-bgl.c — Babylon BGL dictionary parser (UTF-8 transcoded cache)
 */

#include "dict-mmap.h"
#include "flat-index.h"
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
#include <errno.h>
#include <iconv.h>

#include "dict-cache.h"

static const char *bgl_charset[] = {
    "WINDOWS-1252", /* Default */
    "WINDOWS-1252", /* Latin */
    "WINDOWS-1250", /* Eastern European */
    "WINDOWS-1251", /* Cyrillic */
    "CP932",        /* Japanese */
    "BIG5",         /* Traditional Chinese */
    "GB18030",      /* Simplified Chinese */
    "CP1257",       /* Baltic */
    "CP1253",       /* Greek */
    "EUC-KR",       /* Korean */
    "ISO-8859-9",   /* Turkish */
    "WINDOWS-1255", /* Hebrew */
    "CP1256",       /* Arabic */
    "CP874"         /* Thai */
};
#define BGL_CHARSET_COUNT (sizeof(bgl_charset)/sizeof(bgl_charset[0]))

static unsigned int read_be(const unsigned char *p, int bytes) {
    unsigned int val = 0;
    for (int i = 0; i < bytes; i++) val = (val << 8) | p[i];
    return val;
}

static char *find_bgl_resource_dir(const char *path) {
    char *base = g_strdup(path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char *resource_dir = g_strconcat(base, ".files", NULL);
    g_free(base);
    if (g_file_test(resource_dir, G_FILE_TEST_IS_DIR)) return resource_dir;
    g_free(resource_dir);
    return NULL;
}

static void trim_whitespace(char *s, size_t *len) {
    if (!s || *len == 0) return;
    char *start = s;
    char *end = s + (*len) - 1;
    while (start <= end && (unsigned char)*start <= 32) start++;
    while (end >= start && (unsigned char)*end <= 32) end--;
    size_t new_len = (start <= end) ? (end - start + 1) : 0;
    if (new_len < *len) {
        memmove(s, start, new_len);
        s[new_len] = '\0';
        *len = new_len;
    }
}

static char *bgl_decode_string(const char *data, size_t len, const char *charset, size_t *out_len) {
    if (!data || len == 0) {
        if (out_len) *out_len = 0;
        return g_strdup("");
    }
    if (!charset || strcmp(charset, "UTF-8") == 0) {
        if (out_len) *out_len = len;
        return g_strndup(data, len);
    }
    
    iconv_t cd = iconv_open("UTF-8", charset);
    if (cd == (iconv_t)-1) {
        if (out_len) *out_len = len;
        return g_strndup(data, len);
    }
    
    size_t inbytesleft = len;
    char *inbuf = (char *)data;
    
    size_t out_cap = len * 4 + 1;
    char *out_buf = g_malloc(out_cap);
    size_t outbytesleft = out_cap - 1;
    char *outptr = out_buf;
    
    while (inbytesleft > 0) {
        size_t res = iconv(cd, &inbuf, &inbytesleft, &outptr, &outbytesleft);
        if (res == (size_t)-1) {
            if (errno == EILSEQ || errno == EINVAL) {
                inbuf++; inbytesleft--;
            } else if (errno == E2BIG) {
                size_t used = outptr - out_buf;
                out_cap *= 2;
                out_buf = g_realloc(out_buf, out_cap);
                outptr = out_buf + used;
                outbytesleft = out_cap - used - 1;
            } else {
                break;
            }
        }
    }
    *outptr = '\0';
    if (out_len) *out_len = outptr - out_buf;
    iconv_close(cd);
    return out_buf;
}

static char *format_bgl_definition(const unsigned char *def_data, size_t def_len, const char *target_charset, size_t *out_len) {
    if (def_len == 0) { *out_len = 0; return g_strdup(""); }
    
    GString *raw_str = g_string_sized_new(def_len + 64);
    for (size_t i = 0; i < def_len; i++) {
        unsigned char c = def_data[i];
        if (c == 0x0a) {
            g_string_append(raw_str, "<br>");
        } else if (c == 0x06) {
            if (i + 1 < def_len) i++;
            g_string_append_c(raw_str, ' ');
        } else if (c >= 0x40 && (def_len - i) >= 2 && def_data[i+1] == 0x18) {
            unsigned int len = c - 0x3F;
            if (len <= def_len - i - 2) i += len + 1;
        } else if (c == 0x18) {
            unsigned int len = def_data[i+1];
            if (len <= def_len - i - 2) i += len + 1;
        } else if (c == 0x28 && (def_len - i) >= 3) {
            // we should technically check if Previous Body Ended but simplicity rules
            unsigned int len = (def_data[i+1] << 8) | def_data[i+2];
            if (len <= def_len - i - 3) i += len + 2;
        } else if (c == 0x50 && (def_len - i) >= 3 && def_data[i+1] == 0x1B) {
            unsigned int len = def_data[i+2];
            if (len <= def_len - i - 3) i += len + 2;
        } else if (c == 0x60 && (def_len - i) >= 4 && def_data[i+1] == 0x1B) {
            unsigned int len = (def_data[i+2] << 8) | def_data[i+3];
            if (len <= def_len - i - 4) i += len + 3;
        } else if (c >= 0x40 && (def_len - i) >= 2 && def_data[i+1] == 0x1B) {
            unsigned int len = c - 0x3F;
            if (len <= def_len - i - 2) i += len + 1;
        } else if (c == 0x1E) {
            // resource prefix
        } else if (c == 0x1F) {
            // resource suffix
        } else if (c < 0x20) {
            if (i <= def_len - 3 && c == 0x14 && def_data[i+1] == 0x02) {
                // POS
                i += 2;
            } else if (c == 0x14) {
            } else if (c == 0x1A) {
                unsigned int len = def_data[i+1];
                if (len <= 10 && len <= def_len - i - 2) {
                    i += len + 1;
                }
            } else {
                g_string_append_c(raw_str, c);
            }
        } else {
            g_string_append_c(raw_str, c);
        }
    }
    
    char *utf8_def = bgl_decode_string(raw_str->str, raw_str->len, target_charset, out_len);
    g_string_free(raw_str, TRUE);
    return utf8_def;
}

static size_t transcode_bgl_blocks(const char *data, size_t data_size, FILE *out_file,
                                   TreeEntry **out_entries, char **out_name,
                                   volatile gint *cancel_flag, gint expected) {
    const unsigned char *p = (const unsigned char *)data;
    const unsigned char *end = p + data_size;
    int word_count = 0;

    size_t entry_cap = 4096;
    TreeEntry *entries = calloc(entry_cap, sizeof(TreeEntry));
    char *dict_name = NULL;
    
    const char *default_charset = "UTF-8"; // Usually missing metadata means the file was natively compiled as UTF-8
    const char *source_charset_override = NULL;
    const char *target_charset_override = NULL;
    gboolean is_utf8 = FALSE;

    // Scan for charsets first
    const unsigned char *sp = p;
    while (sp < end && !is_utf8) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        if (sp + 1 > end) break;
        unsigned int first_byte = sp[0];
        unsigned int block_type = first_byte & 0x0F;
        if (block_type == 4) break; 
        unsigned int len_code = first_byte >> 4;
        sp++;
        unsigned int block_len;
        if (len_code < 4) {
            int num_bytes = len_code + 1;
            if (sp + num_bytes > end) break;
            block_len = read_be(sp, num_bytes);
            sp += num_bytes;
        } else { block_len = len_code - 4; }
        
        if (block_len == 0 || sp + block_len > end) { sp += block_len; continue; }
        const unsigned char *block_data = sp;
        sp += block_len;
        
        if (block_type == 0 && block_len > 2) {
            if (block_data[0] == 8) {
                unsigned int type = block_data[2];
                if (type > 64) type -= 65;
                if (type >= BGL_CHARSET_COUNT) type = 0;
                default_charset = bgl_charset[type];
            }
        } else if (block_type == 3 && block_len > 1) {
            unsigned int sub = block_data[1]; // FIX: Block 3 metadata subtype is at byte 1, not 0!
            if (sub == 17 && block_len >= 5 && (block_data[4] & 0x80)) is_utf8 = TRUE;
            else if (sub == 26 && block_len >= 3) {
                unsigned int type = block_data[2];
                if (type > 64) type -= 65;
                if (type >= BGL_CHARSET_COUNT) type = 0;
                source_charset_override = bgl_charset[type];
            } else if (sub == 27 && block_len >= 3) {
                unsigned int type = block_data[2];
                if (type > 64) type -= 65;
                if (type >= BGL_CHARSET_COUNT) type = 0;
                target_charset_override = bgl_charset[type];
            } else if (sub == 1 && !dict_name) {
                dict_name = g_strndup((const char *)block_data + 2, block_len - 2);
            }
        }
    }
    
    const char *source_charset = is_utf8 ? "UTF-8" : (source_charset_override ? source_charset_override : default_charset);
    const char *target_charset = is_utf8 ? "UTF-8" : (target_charset_override ? target_charset_override : default_charset);

    // Initial offset in the new out_file
    uint64_t current_file_offset = 8; 

    while (p < end) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) break;
        if (p + 1 > end) break;

        unsigned int first_byte = p[0];
        unsigned int block_type = first_byte & 0x0F;

        if (block_type == 4) break; 

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
            
            const unsigned char *hw_data = block_data + pos;
            pos += hw_len;

            unsigned int def_len;
            if (block_type == 11) {
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

            const unsigned char *def_data = block_data + pos;

            if (hw_len > 0 && def_len > 0) {
                if ((size_t)word_count >= entry_cap) {
                    entry_cap *= 2;
                    entries = realloc(entries, entry_cap * sizeof(TreeEntry));
                }
                
                size_t decoded_hw_len = 0;
                char *hw_utf8 = bgl_decode_string((const char*)hw_data, hw_len, source_charset, &decoded_hw_len);
                if (decoded_hw_len == 0) { g_free(hw_utf8); continue; }

                trim_whitespace(hw_utf8, &decoded_hw_len);

                /* Strip BGL $123$ numeric postfixes if present */
                if (decoded_hw_len > 0 && hw_utf8[decoded_hw_len - 1] == '$') {
                    if (decoded_hw_len >= 2) {
                        int x = (int)decoded_hw_len - 2;
                        while (x >= 0) {
                            if (hw_utf8[x] == '$') {
                                hw_utf8[x] = '\0';
                                decoded_hw_len = (size_t)x;
                                trim_whitespace(hw_utf8, &decoded_hw_len);
                                break;
                            } else if (hw_utf8[x] < '0' || hw_utf8[x] > '9') {
                                break;
                            }
                            x--;
                        }
                    }
                }
                
                size_t decoded_def_len = 0;
                char *def_utf8 = format_bgl_definition(def_data, def_len, target_charset, &decoded_def_len);
                
                // Write null-terminated strings
                fwrite(hw_utf8, 1, decoded_hw_len, out_file);
                fputc(0, out_file);
                fwrite(def_utf8, 1, decoded_def_len, out_file);
                fputc(0, out_file);
                
                entries[word_count].h_off = current_file_offset;
                entries[word_count].h_len = decoded_hw_len;
                entries[word_count].d_off = current_file_offset + decoded_hw_len + 1;
                entries[word_count].d_len = decoded_def_len;
                word_count++;
                
                current_file_offset += decoded_hw_len + 1 + decoded_def_len + 1;
                
                g_free(hw_utf8);
                g_free(def_utf8);
            }
        }
    }

    if (out_name) *out_name = dict_name;
    else g_free(dict_name);
    
    if (word_count > 0) {
        // Read everything back and sort the array by referencing the file offsets?
        // Wait, flat_index_sort_entries needs `data` mapping!
        // We can just sort entries by reading the data from `out_file`.
        *out_entries = entries;
        fflush(out_file);
        // Let the caller handle index sorting since it requires memory map of the new file.
    } else {
        g_free(entries);
        *out_entries = NULL;
    }
    
    printf("[BGL] Transcoded %d entries into cache\n", word_count);
    return (size_t)word_count;
}

DictMmap* parse_bgl_file(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    unsigned char hdr[6];
    if (fread(hdr, 1, 6, f) < 6) { fclose(f); return NULL; }

    if (hdr[0] != 0x12 || hdr[1] != 0x34 || hdr[2] != 0x00 ||
        (hdr[3] != 0x01 && hdr[3] != 0x02)) {
        fprintf(stderr, "[BGL] Invalid signature in %s\n", path);
        fclose(f);
        return NULL;
    }

    int gz_offset = (hdr[4] << 8) | hdr[5];
    if (gz_offset < 6) { fclose(f); return NULL; }

    fseek(f, gz_offset, SEEK_SET);
    int fd_dup = dup(fileno(f));
    lseek(fd_dup, gz_offset, SEEK_SET);
    fclose(f);

    gzFile gz = gzdopen(fd_dup, "rb");
    if (!gz) { close(fd_dup); return NULL; }

    dict_cache_ensure_dir();
    char *cache_path = dict_cache_path_for(path);
    gboolean cache_exists = (access(cache_path, F_OK) == 0);
    gboolean cache_valid = cache_exists && dict_cache_is_valid(cache_path, path);

    int cache_fd = -1;
    const char *dict_data = NULL;
    size_t dict_size = 0;

    if (cache_valid) {
        gzclose(gz);
        printf("[BGL] Loading from cache: %s\n", cache_path);
        cache_fd = open(cache_path, O_RDONLY);
        if (cache_fd < 0) { g_free(cache_path); return NULL; }

        struct stat st;
        if (fstat(cache_fd, &st) < 0 || st.st_size < 16) {
            close(cache_fd); g_free(cache_path); return NULL;
        }
        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
        if (dict_data == MAP_FAILED) {
            close(cache_fd); g_free(cache_path); return NULL;
        }

        DictMmap *dict = g_new0(DictMmap, 1);
        dict->fd = cache_fd;
        dict->tmp_file = NULL;
        dict->data = dict_data;
        dict->size = dict_size;
        dict->resource_dir = find_bgl_resource_dir(path);
        dict->index = flat_index_open(dict->data, dict->size);

        if (!dict->index || dict->index->count == 0) {
            fprintf(stderr, "[BGL] Cache invalid structure. Rebuilding completely.\n");
            flat_index_close(dict->index);
            munmap((void*)dict->data, dict->size);
            close(dict->fd);
            g_free(dict);
            dict_data = NULL;
            // fallthrough and rebuild!
            cache_valid = FALSE;
            fd_dup = open(path, O_RDONLY);
            lseek(fd_dup, gz_offset, SEEK_SET);
            gz = gzdopen(fd_dup, "rb");
        } else {
            g_free(cache_path);
            return dict;
        }
    }

    if (!cache_valid) {
        printf("[BGL] Transcoding and building UTF-8 cache from source: %s\n", path);

        char tmp_raw[256];
        snprintf(tmp_raw, sizeof(tmp_raw), "%s.raw", cache_path);
        FILE *tf = fopen(tmp_raw, "wb");
        if (!tf) { gzclose(gz); g_free(cache_path); return NULL; }

        unsigned char buf[65536];
        int n;
        while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                gzclose(gz); fclose(tf); unlink(tmp_raw); g_free(cache_path); return NULL;
            }
            fwrite(buf, 1, n, tf);
        }
        gzclose(gz);
        fclose(tf);

        int raw_fd = open(tmp_raw, O_RDONLY);
        struct stat raw_st;
        fstat(raw_fd, &raw_st);
        const char *raw_data = mmap(NULL, raw_st.st_size, PROT_READ, MAP_PRIVATE, raw_fd, 0);

        FILE *cache_file = fopen(cache_path, "wb");
        uint64_t zero_count = 0;
        fwrite(&zero_count, 8, 1, cache_file);

        TreeEntry *entries = NULL;
        char *dict_name = NULL;
        size_t entry_count = transcode_bgl_blocks(raw_data, raw_st.st_size, cache_file, &entries, &dict_name, cancel_flag, expected);

        munmap((void*)raw_data, raw_st.st_size);
        close(raw_fd);
        unlink(tmp_raw);

        if (entry_count > 0 && entries) {
            fflush(cache_file);
            long data_end = ftell(cache_file);
            
            // Map the newly written strings to sort the entries!
            int tmp_cache_fd = open(cache_path, O_RDONLY);
            const char *tmp_cache_data = mmap(NULL, data_end, PROT_READ, MAP_PRIVATE, tmp_cache_fd, 0);
            
            flat_index_sort_entries(entries, entry_count, tmp_cache_data, data_end);
            
            munmap((void*)tmp_cache_data, data_end);
            close(tmp_cache_fd);
            
            fseek(cache_file, data_end, SEEK_SET);
            fwrite(entries, sizeof(TreeEntry), entry_count, cache_file);
            fseek(cache_file, 0, SEEK_SET);
            uint64_t final_count = (uint64_t)entry_count;
            fwrite(&final_count, 8, 1, cache_file);
        }
        fclose(cache_file);
        g_free(entries);

        struct stat src_st;
        if (stat(path, &src_st) == 0) {
            struct utimbuf times;
            times.actime = src_st.st_mtime;
            times.modtime = src_st.st_mtime;
            utime(cache_path, &times);
        }

        cache_fd = open(cache_path, O_RDONLY);
        struct stat st;
        fstat(cache_fd, &st);
        dict_size = st.st_size;
        dict_data = mmap(NULL, dict_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);

        DictMmap *dict = g_new0(DictMmap, 1);
        dict->fd = cache_fd;
        dict->tmp_file = NULL;
        dict->data = dict_data;
        dict->size = dict_size;
        dict->name = dict_name;
        dict->resource_dir = find_bgl_resource_dir(path);
        dict->index = flat_index_open(dict->data, dict->size);

        g_free(cache_path);
        printf("[BGL] Loaded %zu entries from %s\n", dict->index ? dict->index->count : 0, path);
        return dict;
    }
    g_free(cache_path);
    return NULL;
}
