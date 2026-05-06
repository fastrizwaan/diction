#include "text-utils.h"

size_t convert_utf16le_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf, uint32_t *offset_map) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len) {
        if (offset_map) offset_map[out] = (uint32_t)in;
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
            if (offset_map) offset_map[out] = (uint32_t)in - 2;
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            if (offset_map) offset_map[out] = (uint32_t)in - 2;
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            if (offset_map) offset_map[out] = (uint32_t)in - 2;
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            if (offset_map) offset_map[out] = (uint32_t)in - 4;
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            if (offset_map) offset_map[out] = (uint32_t)in - 4;
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            if (offset_map) offset_map[out] = (uint32_t)in - 4;
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}

size_t convert_utf16be_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf, uint32_t *offset_map) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len) {
        if (offset_map) offset_map[out] = (uint32_t)in;
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
            if (offset_map) offset_map[out] = (uint32_t)in - 2;
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            if (offset_map) offset_map[out] = (uint32_t)in - 2;
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            if (offset_map) offset_map[out] = (uint32_t)in - 2;
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            if (offset_map) offset_map[out] = (uint32_t)in - 4;
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            if (offset_map) offset_map[out] = (uint32_t)in - 4;
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            if (offset_map) offset_map[out] = (uint32_t)in - 4;
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}
