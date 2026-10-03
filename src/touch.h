// Touch injection: events written into the touchscreen's own evdev node, which MPC reads as a real touch.
#ifndef TOUCH_H
#define TOUCH_H

// Open the touchscreen: dev is a /dev/input/eventN path or "auto" (the first multitouch device that isn't virtual,
// one that says it's on the screen first). rotate (0, 90, 180, 270) is how the panel's axes sit against the
// scanout (normally 0: both are the physical panel). 0 on success.
int touch_open(const char *dev, int rotate);
// The device opened, or "none".
const char *touch_device(void);
// Scanout pixel (x, y) on a w x h scanout to panel coordinates.
void touch_map(int x, int y, int w, int h, int *tx, int *ty);
void touch_down(int x, int y, int w, int h);
void touch_move(int x, int y, int w, int h);
void touch_up(void);

#endif
