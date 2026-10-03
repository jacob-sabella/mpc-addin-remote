// PNG encoding of an RGB24 frame, with zlib loaded at run time (MPC already has libz.so.1 loaded).
#ifndef PNG_H
#define PNG_H
#include <stddef.h>
#include <stdint.h>

// 0 when zlib is available.
int png_init(void);
// A malloc'd PNG of the w x h RGB24 frame (free() it), NULL on failure.
uint8_t *png_encode(const uint8_t *rgb, int w, int h, size_t *len);
// The same at a zlib level (1 fast .. 9 small): MCP screenshots go into a model's context, so smaller is worth it.
uint8_t *png_encode_level(const uint8_t *rgb, int w, int h, size_t *len, int level);
// CRC-32 of a buffer (zlib's), to tell an unchanged frame.
uint32_t png_crc(const uint8_t *p, size_t n);

#endif
