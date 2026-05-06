#ifndef MDX_DECOMPRESS_H
#define MDX_DECOMPRESS_H

#include <glib.h>
#include <stdint.h>

unsigned char *mdx_block_decompress(const unsigned char *block,
                                    size_t comp_size,
                                    size_t decomp_hint,
                                    size_t *out_len);

void mdx_decrypt_key_block_info(unsigned char *buf, size_t len);

#endif
