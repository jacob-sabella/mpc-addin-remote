// Touch: one finger in slot 0 of the multitouch protocol B, plus BTN_TOUCH and ABS_X/ABS_Y for readers that only
// look at single touch. Writing to an evdev node injects the events into that device's stream.
#define _GNU_SOURCE
#include "touch.h"
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int fd = -1;
static int rot;
static int xmin, xmax = 2047, ymin, ymax = 2047;   // the panel's ABS_MT_POSITION_X/Y ranges
static int tracking = 1, is_down;
static char devname[64] = "none";

#define BIT(arr, b) ((arr)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long))) & 1)

// A direct (on-screen) multitouch device that isn't a virtual one (a mouse add-on's uinput touch, say).
static int is_touchscreen(int f)
{
    unsigned long abs[(ABS_MAX + 1) / (8 * sizeof(long)) + 1] = { 0 };
    unsigned long prop[(INPUT_PROP_MAX + 1) / (8 * sizeof(long)) + 1] = { 0 };
    struct input_id id;
    if (ioctl(f, EVIOCGBIT(EV_ABS, sizeof abs), abs) < 0) return 0;
    if (!BIT(abs, ABS_MT_POSITION_X) || !BIT(abs, ABS_MT_POSITION_Y)) return 0;
    if (ioctl(f, EVIOCGPROP(sizeof prop), prop) >= 0 && !BIT(prop, INPUT_PROP_DIRECT)) return 0;
    if (ioctl(f, EVIOCGID, &id) == 0 && id.bustype == BUS_VIRTUAL) return 0;
    return 1;
}

static void ranges(int f)
{
    struct input_absinfo a;
    if (ioctl(f, EVIOCGABS(ABS_MT_POSITION_X), &a) == 0 && a.maximum > a.minimum) { xmin = a.minimum; xmax = a.maximum; }
    if (ioctl(f, EVIOCGABS(ABS_MT_POSITION_Y), &a) == 0 && a.maximum > a.minimum) { ymin = a.minimum; ymax = a.maximum; }
}

int touch_open(const char *dev, int rotate)
{
    pthread_mutex_lock(&mtx);
    rot = (rotate % 360 + 360) % 360 / 90 * 90;
    int r = -1;
#ifdef REMOTE_TEST
    const char *fake = getenv("REMOTE_FAKE_TOUCH");   // test builds: events go to a file, ranges stay 0..2047
    if (fake) {
        fd = open(fake, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd >= 0) { snprintf(devname, sizeof devname, "%s", fake); r = 0; }
        pthread_mutex_unlock(&mtx);
        return r;
    }
#endif
    char p[64];
    for (int i = 0; i < 32 && r; i++) {
        if (dev && strcmp(dev, "auto")) { if (i) break; snprintf(p, sizeof p, "%s", dev); }
        else snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_WRONLY | O_CLOEXEC);   // write-only: a reader would be sent every touch it never reads
        if (f < 0) continue;
        if (!(dev && strcmp(dev, "auto")) && !is_touchscreen(f)) { close(f); continue; }
        ranges(f);
        fd = f;
        snprintf(devname, sizeof devname, "%s", p);
        r = 0;
    }
    pthread_mutex_unlock(&mtx);
    return r;
}

const char *touch_device(void)
{
    return devname;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Scale 0..n-1 onto lo..hi (n positions onto the panel's span, as the panel driver reports them).
static int scale(int v, int n, int lo, int hi)
{
    return clampi(lo + (int)((long long)v * (hi - lo + 1) / n), lo, hi);
}

void touch_map(int x, int y, int w, int h, int *tx, int *ty)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    x = clampi(x, 0, w - 1);
    y = clampi(y, 0, h - 1);
    switch (rot) {
    case 90:  *tx = scale(y, h, xmin, xmax); *ty = scale(w - x, w, ymin, ymax); break;
    case 180: *tx = scale(w - x, w, xmin, xmax); *ty = scale(h - y, h, ymin, ymax); break;
    case 270: *tx = scale(h - y, h, xmin, xmax); *ty = scale(x, w, ymin, ymax); break;
    default:  *tx = scale(x, w, xmin, xmax); *ty = scale(y, h, ymin, ymax); break;
    }
}

static void ev(int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof e);      // the kernel stamps injected events itself
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (fd >= 0 && write(fd, &e, sizeof e) < 0) { }
}

static void lift(void)
{
    ev(EV_ABS, ABS_MT_SLOT, 0);
    ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ev(EV_KEY, BTN_TOUCH, 0);
    ev(EV_SYN, SYN_REPORT, 0);
    is_down = 0;
}

void touch_down(int x, int y, int w, int h)
{
    pthread_mutex_lock(&mtx);
    int tx, ty;
    touch_map(x, y, w, h, &tx, &ty);
    if (is_down) lift();
    ev(EV_ABS, ABS_MT_SLOT, 0);
    ev(EV_ABS, ABS_MT_TRACKING_ID, tracking);
    tracking = tracking >= 0xfffff ? 1 : tracking + 1;
    ev(EV_ABS, ABS_MT_POSITION_X, tx);
    ev(EV_ABS, ABS_MT_POSITION_Y, ty);
    ev(EV_KEY, BTN_TOUCH, 1);
    ev(EV_ABS, ABS_X, tx);
    ev(EV_ABS, ABS_Y, ty);
    ev(EV_SYN, SYN_REPORT, 0);
    is_down = 1;
    pthread_mutex_unlock(&mtx);
}

void touch_move(int x, int y, int w, int h)
{
    pthread_mutex_lock(&mtx);
    if (is_down) {
        int tx, ty;
        touch_map(x, y, w, h, &tx, &ty);
        ev(EV_ABS, ABS_MT_SLOT, 0);
        ev(EV_ABS, ABS_MT_POSITION_X, tx);
        ev(EV_ABS, ABS_MT_POSITION_Y, ty);
        ev(EV_ABS, ABS_X, tx);
        ev(EV_ABS, ABS_Y, ty);
        ev(EV_SYN, SYN_REPORT, 0);
    }
    pthread_mutex_unlock(&mtx);
}

void touch_up(void)
{
    pthread_mutex_lock(&mtx);
    if (is_down) lift();
    pthread_mutex_unlock(&mtx);
}
