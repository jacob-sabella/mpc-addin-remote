// Screenshots for a model: a region of the upright screen at a zoom, with an optional grid labelled in screen
// pixels, so the coordinates to touch can be read off the picture.
#ifndef IMAGE_H
#define IMAGE_H
#include <stdint.h>

// Nearest-neighbour copy of the region (rx, ry, rw, rh) of a w-wide RGB24 frame, at zoom z2 / 2 (z2 1..8: half size
// to 4x). The output is (rw * z2 / 2) x (rh * z2 / 2), at least 1 x 1; *ow, *oh get it. NULL if out of memory.
uint8_t *img_region(const uint8_t *rgb, int w, int rx, int ry, int rw, int rh, int z2, int *ow, int *oh);
// The grid step in screen pixels for a zoom: lines at least about 64 image pixels apart.
int img_grid_step(int z2);
// Lines every `step` screen pixels over an image made by img_region(rx, ry, ..., z2), each labelled with its screen
// coordinate (x along the top, y down the left side).
void img_grid(uint8_t *img, int ow, int oh, int rx, int ry, int z2, int step);

#endif
