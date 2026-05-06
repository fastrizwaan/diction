#pragma once
#include <stddef.h>
#include <stdint.h>

size_t convert_utf16le_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf, uint32_t *offset_map);
size_t convert_utf16be_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf, uint32_t *offset_map);
