// Screen capture: reads the framebuffer the display is scanning out, through the DRM device, as RGB24.
#ifndef CAPTURE_H
#define CAPTURE_H
#include <stddef.h>
#include <stdint.h>

// The current screen size; 0 on success, a negative code when no display is found (see cap_error()).
int cap_size(int *w, int *h);
// One frame as RGB24, every `scale`th pixel and row (1 full, 2 half, up to 4). out holds at least
// (w/scale)*(h/scale)*3 bytes for the size cap_size() gave; *ow and *oh get the frame's size. 0 on success.
int cap_frame(uint8_t *out, size_t outsz, int scale, int *ow, int *oh);
// The scanout's pixel format as four characters ("XR24"), or "none".
const char *cap_format(void);
// Text for a negative return code.
const char *cap_error(int rc);

#endif
