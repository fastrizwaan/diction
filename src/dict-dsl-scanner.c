#include "dict-dsl-scanner.h"
#include <stdlib.h>
#include <string.h>

static size_t dsl_convert_utf16le_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf, size_t out_max) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len && out + 4 <= out_max) {
        uint32_t wc = in_buf[in] | (in_buf[in+1] << 8);
        in += 2;
        if (wc >= 0xD800 && wc <= 0xDBFF && in + 1 < in_len) {
            uint32_t wc2 = in_buf[in] | (in_buf[in+1] << 8);
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                in += 2;
                wc = 0x10000 + ((wc & 0x3FF) << 10) + (wc2 & 0x3FF);
            }
        }
        if (wc < 0x80) { out_buf[out++] = wc; }
        else if (wc < 0x800) {
            out_buf[out++] = 0xC0 | (wc >> 6);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}

static size_t dsl_convert_utf16be_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf, size_t out_max) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len && out + 4 <= out_max) {
        uint32_t wc = (in_buf[in] << 8) | in_buf[in+1];
        in += 2;
        if (wc >= 0xD800 && wc <= 0xDBFF && in + 1 < in_len) {
            uint32_t wc2 = (in_buf[in] << 8) | in_buf[in+1];
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                in += 2;
                wc = 0x10000 + ((wc & 0x3FF) << 10) + (wc2 & 0x3FF);
            }
        }
        if (wc < 0x80) { out_buf[out++] = wc; }
        else if (wc < 0x800) {
            out_buf[out++] = 0xC0 | (wc >> 6);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}

DslScanner* dsl_scanner_open(const char *path) {
    DslScanner *s = calloc(1, sizeof(DslScanner));
    s->buf_cap = 1048576; // Start with 1MB buffer
    s->buf = malloc(s->buf_cap);
    
    size_t len = strlen(path);
    if (len > 3 && strcasecmp(path + len - 3, ".dz") == 0) {
        s->is_compressed = 1;
        s->gz = gzopen(path, "rb");
        if (!s->gz) { free(s); return NULL; }
    } else {
        s->is_compressed = 0;
        s->f = fopen(path, "rb");
        if (!s->f) { free(s); return NULL; }
    }
    
    unsigned char bom[4];
    int bom_len;
    if (s->is_compressed) {
        bom_len = gzread(s->gz, bom, 4);
    } else {
        bom_len = fread(bom, 1, 4, s->f);
    }

    if (bom_len < 2) {
        if (s->is_compressed) gzclose(s->gz);
        else fclose(s->f);
        free(s);
        return NULL;
    }
    
    int copy_offset = 0;
    if (bom[0] == 0xFF && bom[1] == 0xFE) {
        s->is_utf16le = 1;
        copy_offset = 2;
    } else if (bom[0] == 0xFE && bom[1] == 0xFF) {
        s->is_utf16be = 1;
        copy_offset = 2;
    } else if (bom_len >= 4 && bom[0] != 0 && bom[1] == 0 && bom[2] != 0 && bom[3] == 0) {
        s->is_utf16le = 1;
    } else if (bom_len >= 4 && bom[0] == 0 && bom[1] != 0 && bom[2] == 0 && bom[3] != 0) {
        s->is_utf16be = 1;
    } else if (bom_len >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        copy_offset = 3;
    }
    
    s->uncomp_offset = copy_offset;
    if (s->is_compressed) {
        gzseek(s->gz, copy_offset, SEEK_SET);
    } else {
        fseek(s->f, copy_offset, SEEK_SET);
    }
    return s;
}

static int fill_buffer(DslScanner *s) {
    if (s->eof) return 0;
    if (s->buf_pos > 0) {
        size_t rem = s->buf_len - s->buf_pos;
        if (rem > 0) memmove(s->buf, s->buf + s->buf_pos, rem);
        s->buf_len = rem;
        s->buf_pos = 0;
    }
    if (s->buf_len >= s->buf_cap) {
        s->buf_cap *= 2;
        s->buf = realloc(s->buf, s->buf_cap);
    }
    int read_bytes;
    if (s->is_compressed) {
        read_bytes = gzread(s->gz, s->buf + s->buf_len, s->buf_cap - s->buf_len);
    } else {
        read_bytes = fread(s->buf + s->buf_len, 1, s->buf_cap - s->buf_len, s->f);
    }
    if (read_bytes <= 0) {
        s->eof = 1;
        return 0;
    }
    s->buf_len += read_bytes;
    return 1;
}

int dsl_scanner_read_line(DslScanner *s, char *out_utf8, size_t out_max, size_t *out_len, uint64_t *out_uncomp_offset, size_t *out_uncomp_len) {
    while (1) {
        size_t avail = s->buf_len - s->buf_pos;
        if (avail == 0) {
            if (!fill_buffer(s)) {
                return 0; // EOF
            }
            continue;
        }

        int char_size = (s->is_utf16le || s->is_utf16be) ? 2 : 1;
        size_t i = s->buf_pos;
        int found_nl = 0;
        
        while (i + char_size <= s->buf_len) {
            if (char_size == 1) {
                if (s->buf[i] == '\n') { found_nl = 1; i++; break; }
                i++;
            } else {
                uint16_t ch = s->is_utf16le ? (s->buf[i] | (s->buf[i+1] << 8)) : ((s->buf[i] << 8) | s->buf[i+1]);
                if (ch == '\n') { found_nl = 1; i += 2; break; }
                i += 2;
            }
        }
        
        if (!found_nl) {
            if (!s->eof) {
                fill_buffer(s);
                continue;
            } else {
                i = s->buf_len; // EOF reached, return rest of buffer
                if (i == s->buf_pos) return 0;
            }
        }

        size_t raw_len = i - s->buf_pos;
        *out_uncomp_offset = s->uncomp_offset;
        *out_uncomp_len = raw_len;

        if (s->is_utf16le) {
            *out_len = dsl_convert_utf16le_to_utf8(s->buf + s->buf_pos, raw_len, (unsigned char*)out_utf8, out_max);
        } else if (s->is_utf16be) {
            *out_len = dsl_convert_utf16be_to_utf8(s->buf + s->buf_pos, raw_len, (unsigned char*)out_utf8, out_max);
        } else {
            *out_len = raw_len < out_max ? raw_len : out_max;
            memcpy(out_utf8, s->buf + s->buf_pos, *out_len);
        }
        
        s->buf_pos = i;
        s->uncomp_offset += raw_len;
        
        if (*out_len > 0 && out_utf8[*out_len - 1] == '\n') (*out_len)--;
        if (*out_len > 0 && out_utf8[*out_len - 1] == '\r') (*out_len)--;
        out_utf8[*out_len] = '\0';
        
        return 1;
    }
}

void dsl_scanner_close(DslScanner *s) {
    if (s) {
        if (s->is_compressed && s->gz) gzclose(s->gz);
        else if (!s->is_compressed && s->f) fclose(s->f);
        free(s->buf);
        free(s);
    }
}
