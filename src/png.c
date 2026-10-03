// PNG: one IDAT of filter-type-0 rows, compressed at level 1 (a screen of flat colours compresses well and fast).
#define _GNU_SOURCE
#include "png.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static int (*z_compress2)(uint8_t *, unsigned long *, const uint8_t *, unsigned long, int);
static unsigned long (*z_compressBound)(unsigned long);
static unsigned long (*z_crc32)(unsigned long, const uint8_t *, unsigned);
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
    void *z = dlopen("libz.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!z) z = dlopen("libz.so.1", RTLD_NOW);
    if (!z) return;
    z_compressBound = (unsigned long (*)(unsigned long))dlsym(z, "compressBound");
    z_crc32 = (unsigned long (*)(unsigned long, const uint8_t *, unsigned))dlsym(z, "crc32");
    z_compress2 = (int (*)(uint8_t *, unsigned long *, const uint8_t *, unsigned long, int))dlsym(z, "compress2");
}

int png_init(void)
{
    pthread_once(&once, load);
    return z_compress2 && z_compressBound && z_crc32 ? 0 : -1;
}

uint32_t png_crc(const uint8_t *p, size_t n)
{
    unsigned long c = 0;
    while (n) {
        unsigned k = n > 1u << 30 ? 1u << 30 : (unsigned)n;
        c = z_crc32(c, p, k);
        p += k;
        n -= k;
    }
    return (uint32_t)c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// A chunk at p: length, type, data (already in place after the type), CRC. Returns its total size.
static size_t chunk(uint8_t *p, const char *type, size_t n)
{
    be32(p, (uint32_t)n);
    memcpy(p + 4, type, 4);
    be32(p + 8 + n, (uint32_t)z_crc32(0, p + 4, (unsigned)(4 + n)));
    return 12 + n;
}

uint8_t *png_encode(const uint8_t *rgb, int w, int h, size_t *len)
{
    if (png_init() || w <= 0 || h <= 0) return NULL;
    size_t rawlen = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = malloc(rawlen);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (1 + (size_t)w * 3);
        r[0] = 0;
        memcpy(r + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    unsigned long clen = z_compressBound(rawlen);
    uint8_t *png = malloc(8 + 25 + 12 + clen + 12);
    if (!png) { free(raw); return NULL; }
    size_t o = 0;
    memcpy(png, "\x89PNG\r\n\x1a\n", 8);
    o += 8;
    uint8_t *ih = png + o + 8;                       // IHDR: size, 8-bit RGB, no interlace
    be32(ih, (uint32_t)w);
    be32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 2; ih[10] = 0; ih[11] = 0; ih[12] = 0;
    o += chunk(png + o, "IHDR", 13);
    if (z_compress2(png + o + 8, &clen, raw, rawlen, 1) != 0) { free(raw); free(png); return NULL; }
    free(raw);
    o += chunk(png + o, "IDAT", clen);
    o += chunk(png + o, "IEND", 0);
    *len = o;
    return png;
}
