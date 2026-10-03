// Screen capture: reads the framebuffer the display is scanning out, through the DRM device, as RGB24.
#ifndef CAPTURE_H
#define CAPTURE_H
#include <stddef.h>
#include <stdint.h>

// How the scanout is turned upright, clockwise: 0, 90, 180 or 270, or -1 (auto: a portrait scanout turns 90,
// since MPC's UI is landscape). The panel's own orientation is what the display scans out; the UI may be drawn
// rotated into it (the MPC Key 37's 800 x 1280 panel).
void cap_set_rotation(int degrees);
// The upright screen size; 0 on success, a negative code when no display is found (see cap_error()).
int cap_size(int *w, int *h);
// One upright frame as RGB24, every `scale`th pixel and row (1 full, 2 half, up to 4). out holds at least
// (w/scale)*(h/scale)*3 bytes for the size cap_size() gave; *ow and *oh get the frame's size. 0 on success.
int cap_frame(uint8_t *out, size_t outsz, int scale, int *ow, int *oh);
// An upright screen point in scanout coordinates (the touch panel's frame), with the scanout's size.
int cap_to_scanout(int x, int y, int *sx, int *sy, int *sw, int *sh);
// Milliseconds the last cap_frame() took (reading the buffer and converting).
int cap_last_ms(void);
// The scanout's pixel format as four characters ("XR24"), or "none".
const char *cap_format(void);
// Text for a negative return code.
const char *cap_error(int rc);

#endif
