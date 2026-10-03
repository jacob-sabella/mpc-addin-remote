#include "image.h"
#include <stdlib.h>
#include <string.h>

uint8_t *img_region(const uint8_t *rgb, int w, int rx, int ry, int rw, int rh, int z2, int *ow, int *oh)
{
    int W = rw * z2 / 2, H = rh * z2 / 2;
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    uint8_t *out = malloc((size_t)W * H * 3);
    if (!out) return NULL;
    for (int v = 0; v < H; v++) {
        int sy = ry + v * 2 / z2;
        if (sy > ry + rh - 1) sy = ry + rh - 1;
        const uint8_t *src = rgb + (size_t)sy * w * 3;
        uint8_t *dst = out + (size_t)v * W * 3;
        for (int u = 0; u < W; u++) {
            int sx = rx + u * 2 / z2;
            if (sx > rx + rw - 1) sx = rx + rw - 1;
            memcpy(dst + u * 3, src + sx * 3, 3);
        }
    }
    *ow = W;
    *oh = H;
    return out;
}

int img_grid_step(int z2)
{
    static const int steps[] = { 10, 20, 25, 50, 100, 200, 400 };
    for (unsigned i = 0; i < sizeof steps / sizeof *steps; i++)
        if (steps[i] * z2 / 2 >= 64) return steps[i];
    return 400;
}

// 3 x 5 digits, a row per 3 bits (the top bit is the left column).
static const uint8_t DIGITS[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
    { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};
#define DOT 2                 // each font pixel is DOT x DOT image pixels
#define ADV (4 * DOT)         // a digit's advance

static void put(uint8_t *img, int ow, int oh, int x, int y, const uint8_t c[3])
{
    if (x < 0 || y < 0 || x >= ow || y >= oh) return;
    memcpy(img + ((size_t)y * ow + x) * 3, c, 3);
}

static void blend(uint8_t *img, int ow, int oh, int x, int y, const uint8_t c[3])
{
    if (x < 0 || y < 0 || x >= ow || y >= oh) return;
    uint8_t *p = img + ((size_t)y * ow + x) * 3;
    for (int k = 0; k < 3; k++) p[k] = (uint8_t)((p[k] + c[k] * 3) / 4);
}

// The number n with its top left at (x, y), yellow on a black box.
static void label(uint8_t *img, int ow, int oh, int x, int y, int n)
{
    static const uint8_t ink[3] = { 255, 230, 0 }, box[3] = { 0, 0, 0 };
    char s[12];
    int len = 0;
    do { s[len++] = (char)('0' + n % 10); n /= 10; } while (n && len < 11);
    int bw = len * ADV + DOT, bh = 7 * DOT;
    if (x + bw > ow) x = ow - bw;              // keep the label inside the picture
    if (y + bh > oh) y = oh - bh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) put(img, ow, oh, x + i, y + j, box);
    for (int d = 0; d < len; d++) {
        const uint8_t *g = DIGITS[s[len - 1 - d] - '0'];
        int gx = x + DOT + d * ADV, gy = y + DOT;
        for (int r = 0; r < 5; r++)
            for (int col = 0; col < 3; col++)
                if (g[r] >> (2 - col) & 1)
                    for (int a = 0; a < DOT; a++)
                        for (int b = 0; b < DOT; b++) put(img, ow, oh, gx + col * DOT + b, gy + r * DOT + a, ink);
    }
}

void img_grid(uint8_t *img, int ow, int oh, int rx, int ry, int z2, int step)
{
    static const uint8_t line[3] = { 0, 255, 255 };
    if (step < 1) return;
    int rw = ow * 2 / z2 + 1, rh = oh * 2 / z2 + 1;      // the screen span shown (a pixel over is harmless)
    for (int g = (rx + step - 1) / step * step; g < rx + rw; g += step) {
        int u = (g - rx) * z2 / 2;
        if (u >= ow) break;
        for (int v = 0; v < oh; v++) blend(img, ow, oh, u, v, line);
    }
    for (int g = (ry + step - 1) / step * step; g < ry + rh; g += step) {
        int v = (g - ry) * z2 / 2;
        if (v >= oh) break;
        for (int u = 0; u < ow; u++) blend(img, ow, oh, u, v, line);
    }
    // labels after the lines, so a line never crosses a number
    for (int g = (rx + step - 1) / step * step; g < rx + rw; g += step) {
        int u = (g - rx) * z2 / 2;
        if (u >= ow) break;
        label(img, ow, oh, u + 2, 0, g);
    }
    for (int g = (ry + step - 1) / step * step; g < ry + rh; g += step) {
        int v = (g - ry) * z2 / 2;
        if (v >= oh) break;
        if (v < 7 * DOT + 2) continue;           // the top row of labels is there
        label(img, ow, oh, 0, v + 2, g);
    }
}
