// The MCP endpoint: JSON-RPC 2.0 requests POSTed to /mcp, one JSON reply each (Streamable HTTP without SSE). The
// initialize handshake (protocol revisions 2025-03-26 to 2025-11-25); newer clients probe server/discover first and
// fall back to it when that's answered "method not found". Tools: the screen, the touchscreen, MIDI into MPC, the
// device's status and its files (read-only). Every tool call runs on the connection's own addin thread.
#define _GNU_SOURCE
#include "mcp.h"
#include "capture.h"
#include "device.h"
#include "image.h"
#include "json.h"
#include "midi.h"
#include "png.h"
#include "touch.h"
#include "version.h"
#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mcp_tools.h"

#define MAX_TOKENS 512
#define MAX_SIDE 2560          // a screenshot's longest side
#define PNG_LEVEL 6            // screenshots go into a model's context: smaller beats faster here

static const char *const VERSIONS[] = { "2025-11-25", "2025-06-18", "2025-03-26" };   // newest first

static const char INSTRUCTIONS[] =
    "This server is inside an Akai MPC standalone device: it shows the MPC's screen and touches its touchscreen "
    "like a finger, plays MIDI into MPC, and reads the device's status and files. Start with screenshot. Every "
    "coordinate is a pixel of the full-size screen with the origin at the top left (get_screen_info gives the size), "
    "whatever zoom a screenshot was taken at; screenshot grid=true labels them. Touch tools return a screenshot once "
    "the screen settles, and say whether it changed, so a separate screenshot is rarely needed. The hardware buttons, "
    "pads and knobs can't be pressed from here: use the touchscreen, or play_notes for notes. Changes are real: this "
    "is someone's instrument, so don't delete or overwrite anything unless asked.";

static pthread_mutex_t gesture = PTHREAD_MUTEX_INITIALIZER;
void gesture_lock(void) { pthread_mutex_lock(&gesture); }
void gesture_unlock(void) { pthread_mutex_unlock(&gesture); }

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    if (ms > 0) nanosleep(&(struct timespec){ ms / 1000, (long)(ms % 1000) * 1000000 }, NULL);
}

// ---- a tool's result: content items and whether it failed ----
struct result {
    struct sb b;     // the content items, comma-separated
    int items, error;
};

static void add_text(struct result *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_text(struct result *r, const char *fmt, ...)
{
    struct sb t = { 0 };
    char small[512];
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(small, sizeof small, fmt, a);
    va_end(a);
    if (k < 0) return;
    if ((size_t)k < sizeof small) sb_puts(&t, small);
    else {
        char *big = malloc((size_t)k + 1);
        if (!big) return;
        va_start(a, fmt);
        vsnprintf(big, (size_t)k + 1, fmt, a);
        va_end(a);
        sb_puts(&t, big);
        free(big);
    }
    if (r->items++) sb_puts(&r->b, ",");
    sb_puts(&r->b, "{\"type\":\"text\",\"text\":");
    sb_jstr(&r->b, t.p ? t.p : "");
    sb_puts(&r->b, "}");
    sb_free(&t);
}

static void add_sb_text(struct result *r, struct sb *t)
{
    if (r->items++) sb_puts(&r->b, ",");
    sb_puts(&r->b, "{\"type\":\"text\",\"text\":");
    sb_jstr(&r->b, t->p ? t->p : "");
    sb_puts(&r->b, "}");
}

static void fail(struct result *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(struct result *r, const char *fmt, ...)
{
    char m[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(m, sizeof m, fmt, a);
    va_end(a);
    add_text(r, "%s", m);
    r->error = 1;
}

// ---- arguments ----
struct args {
    const char *s;
    const struct jtok *t;
    int obj;              // the arguments object, or -1 (none given)
    char err[256];
};

static int arg(struct args *a, const char *key) { return a->obj < 0 ? -1 : json_get(a->s, a->t, a->obj, key); }

// An integer argument: def when absent (REQUIRED: must be given). 0 on success, else a->err says why.
#define REQUIRED (-2147483647 - 1)
static int arg_int(struct args *a, const char *key, int def, int lo, int hi, int *out)
{
    int i = arg(a, key);
    double v;
    if (i < 0 || a->t[i].type == J_NULL) {
        if (def == REQUIRED) { snprintf(a->err, sizeof a->err, "%s is required", key); return -1; }
        *out = def;
        return 0;
    }
    if (json_num(a->s, a->t, i, &v) || v != (double)(long long)v || v < lo || v > hi) {
        snprintf(a->err, sizeof a->err, "%s must be a whole number from %d to %d", key, lo, hi);
        return -1;
    }
    *out = (int)v;
    return 0;
}

// A string argument, one of a list (NULL-terminated) when choices is given. *out is "" when absent and not required.
static int arg_str(struct args *a, const char *key, int required, const char *const *choices, char *out, size_t n)
{
    int i = arg(a, key);
    out[0] = 0;
    if (i < 0 || a->t[i].type == J_NULL) {
        if (required) { snprintf(a->err, sizeof a->err, "%s is required", key); return -1; }
        return 0;
    }
    char v[1024];
    if (json_str(a->s, a->t, i, v, sizeof v)) { snprintf(a->err, sizeof a->err, "%s must be a string (at most 1000 bytes)", key); return -1; }
    if (strlen(v) < n) memcpy(out, v, strlen(v) + 1);
    else if (!choices) { snprintf(a->err, sizeof a->err, "%s is too long: at most %zu bytes", key, n - 1); return -1; }
    if (choices) {
        int ok = 0;
        for (int c = 0; choices[c]; c++) ok |= !strcmp(v, choices[c]);
        if (ok) return 0;
        int k = snprintf(a->err, sizeof a->err, "%s must be one of:", key);
        for (int c = 0; choices[c] && k < (int)sizeof a->err; c++)
            k += snprintf(a->err + k, sizeof a->err - (size_t)k, " %s", choices[c]);
        return -1;
    }
    return 0;
}

static int arg_bool(struct args *a, const char *key, int def, int *out)
{
    int i = arg(a, key);
    if (i < 0 || a->t[i].type == J_NULL) { *out = def; return 0; }
    if (json_bool(a->s, a->t, i, out)) { snprintf(a->err, sizeof a->err, "%s must be true or false", key); return -1; }
    return 0;
}

// The screenshot mode: 0 none, 1 half (z2 = 1), 2 full (z2 = 2). Booleans are taken too: true is half.
static int arg_shot(struct args *a, int def, int *out)
{
    int i = arg(a, "screenshot"), b;
    if (i >= 0 && (a->t[i].type == J_TRUE || a->t[i].type == J_FALSE)) { json_bool(a->s, a->t, i, &b); *out = b; return 0; }
    static const char *const modes[] = { "none", "half", "full", NULL };
    char m[8];
    if (arg_str(a, "screenshot", 0, modes, m, sizeof m)) return -1;
    *out = !m[0] ? def : !strcmp(m, "none") ? 0 : !strcmp(m, "half") ? 1 : 2;
    return 0;
}

// A note: 0..127 or a name, C3 = 60 (MPC's naming): letter, optional # or b, octave -2..8.
static int parse_note(const char *s, int *out)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    char c = (char)toupper((unsigned char)s[0]);
    if (c >= '0' && c <= '9') {
        char *end;
        long v = strtol(s, &end, 10);
        if (*end || v < 0 || v > 127) return -1;
        *out = (int)v;
        return 0;
    }
    if (c < 'A' || c > 'G') return -1;
    int n = base[c - 'A'], i = 1;
    if (s[i] == '#' || s[i] == 's') { n++; i++; }
    else if (s[i] == 'b') { n--; i++; }
    char *end;
    long oct = strtol(s + i, &end, 10);
    if (end == s + i || *end || oct < -2 || oct > 9) return -1;
    long v = (oct + 2) * 12 + n;
    if (v < 0 || v > 127) return -1;
    *out = (int)v;
    return 0;
}

static int arg_note(struct args *a, int i, const char *what, int *out)
{
    double v;
    char s[16];
    if (i >= 0 && a->t[i].type == J_NUM && json_num(a->s, a->t, i, &v) == 0 && v == (int)v && v >= 0 && v <= 127) {
        *out = (int)v;
        return 0;
    }
    if (i >= 0 && json_str(a->s, a->t, i, s, sizeof s) == 0 && parse_note(s, out) == 0) return 0;
    snprintf(a->err, sizeof a->err, "%s must be a note number 0-127 or a name such as C3, F#2, Bb4 (C3 = 60)", what);
    return -1;
}

static const char *note_name(int n, char *buf)
{
    static const char *const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    n &= 127;
    snprintf(buf, 8, "%s%d", names[n % 12], n / 12 - 2);   // buf holds 8: "C#-2" at most
    return buf;
}

// ---- the screen ----
struct frame {
    uint8_t *rgb;
    int w, h;
};

static int grab(struct frame *f, int scale)
{
    f->rgb = NULL;
    int w, h, r = cap_size(&w, &h);
    if (r) return r;
    size_t sz = (size_t)w * h * 3;
    f->rgb = malloc(sz);
    if (!f->rgb) return 1;
    r = cap_frame(f->rgb, sz, scale, &f->w, &f->h);
    if (r) { free(f->rgb); f->rgb = NULL; }
    return r;
}

static int same(const struct frame *a, const struct frame *b)
{
    return a->rgb && b->rgb && a->w == b->w && a->h == b->h && !memcmp(a->rgb, b->rgb, (size_t)a->w * a->h * 3);
}

// The box around the pixels that differ (full-size frames). 0 if none do.
static int diff_box(const struct frame *a, const struct frame *b, int box[4])
{
    if (!a->rgb || !b->rgb || a->w != b->w || a->h != b->h) {
        box[0] = 0; box[1] = 0; box[2] = b->w - 1; box[3] = b->h - 1;
        return 1;
    }
    int x0 = a->w, y0 = a->h, x1 = -1, y1 = -1;
    for (int y = 0; y < a->h; y++) {
        const uint8_t *p = a->rgb + (size_t)y * a->w * 3, *q = b->rgb + (size_t)y * a->w * 3;
        if (!memcmp(p, q, (size_t)a->w * 3)) continue;
        if (y < y0) y0 = y;
        y1 = y;
        for (int x = 0; x < a->w; x++)
            if (memcmp(p + x * 3, q + x * 3, 3)) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
    }
    if (y1 < 0) return 0;
    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
    return 1;
}

// An image content item of a region of a full-size frame.
static int add_image(struct result *r, const struct frame *f, int rx, int ry, int rw, int rh, int z2, int grid, int *ow, int *oh)
{
    uint8_t *img = img_region(f->rgb, f->w, rx, ry, rw, rh, z2, ow, oh);
    if (!img) return -1;
    if (grid) img_grid(img, *ow, *oh, rx, ry, z2, img_grid_step(z2));
    size_t len = 0;
    uint8_t *png = png_encode_level(img, *ow, *oh, &len, PNG_LEVEL);
    free(img);
    if (!png) return -1;
    if (r->items++) sb_puts(&r->b, ",");
    sb_puts(&r->b, "{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"");
    sb_base64(&r->b, png, len);
    sb_puts(&r->b, "\"}");
    free(png);
    return 0;
}

static const char *screen_error(int rc)
{
    return rc < 0 ? cap_error(rc) : rc == 1 ? "out of memory" : "the screen can't be read";
}

static int need_png(struct result *r)
{
    if (png_init() == 0) return 0;
    fail(r, "PNG encoding isn't available (libz.so.1 wasn't found)");
    return -1;
}

static void shot_full(struct result *r, const struct frame *f, int mode)
{
    if (!mode || !f->rgb || png_init()) return;
    int ow, oh;
    if (add_image(r, f, 0, 0, f->w, f->h, mode == 2 ? 2 : 1, 0, &ow, &oh) == 0 && mode == 1)
        add_text(r, "(Half-size screenshot: %d x %d screen pixels shown at %d x %d. Coordinates stay in screen pixels.)",
                 f->w, f->h, ow, oh);
}

// Wait for the screen to stop changing: polled at half size every 100 ms, from 150 ms after the action, at most
// max_ms. Returns the time it took, or -1 if it kept changing.
static int settle(int max_ms)
{
    long long t0 = now_ms();
    sleep_ms(150);
    struct frame prev = { 0 }, cur = { 0 };
    grab(&prev, 2);
    int took = -1;
    while (now_ms() - t0 < max_ms) {
        sleep_ms(100);
        if (grab(&cur, 2)) break;
        if (same(&prev, &cur)) { took = (int)(now_ms() - t0); free(cur.rgb); break; }
        free(prev.rgb);
        prev = cur;
        cur.rgb = NULL;
    }
    free(prev.rgb);
    return took;
}

// After a touch: settle, then say whether and where the screen changed since `before`, with a screenshot.
static void report_change(struct result *r, struct frame *before, int mode, const char *did)
{
    int took = settle(2000);
    struct frame after;
    int rc = grab(&after, 1);
    if (rc) { add_text(r, "%s. The screen can't be read now: %s.", did, screen_error(rc)); return; }
    int box[4];
    if (!before->rgb) add_text(r, "%s.", did);
    else if (!diff_box(before, &after, box)) add_text(r, "%s. The screen did not change.", did);
    else
        add_text(r, "%s. The screen changed in x %d-%d, y %d-%d%s.", did, box[0], box[2], box[1], box[3],
                 took < 0 ? " and was still changing 2 s later (an animation, meters or a playing sequence?)" : "");
    shot_full(r, &after, mode);
    free(after.rgb);
}

// The point must be on the screen; *w and *h get the screen size.
static int on_screen(struct result *r, int x, int y, int *w, int *h)
{
    int rc = cap_size(w, h);
    if (rc) { fail(r, "The screen can't be read: %s.", screen_error(rc)); return -1; }
    if (x < 0 || y < 0 || x >= *w || y >= *h) {
        fail(r, "(%d, %d) is off the screen: it is %d x %d, so x is 0 to %d and y 0 to %d.", x, y, *w, *h, *w - 1, *h - 1);
        return -1;
    }
    return 0;
}

static int touch_ready(struct result *r)
{
    if (strcmp(touch_device(), "none")) return 0;
    fail(r, "No touchscreen was found, so touch is off (see get_screen_info).");
    return -1;
}

// Touch at an upright screen point (the panel is in the scanout's frame).
static void finger(int down_or_move, int x, int y)
{
    int sx, sy, sw, sh;
    if (cap_to_scanout(x, y, &sx, &sy, &sw, &sh)) return;
    if (down_or_move == 0) touch_down(sx, sy, sw, sh);
    else touch_move(sx, sy, sw, sh);
}

// A slide from (x0, y0) to (x1, y1) over ms, the finger already down.
static void slide(int x0, int y0, int x1, int y1, int ms)
{
    int steps = ms / 16;
    if (steps < 2) steps = 2;
    for (int i = 1; i <= steps; i++) {
        sleep_ms(ms / steps);
        finger(1, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
    }
}

// ---- the tools ----
static void t_screenshot(struct args *a, struct result *r)
{
    int x, y, w = 0, h = 0, grid, sw, sh;
    double zoom = 1;
    if (arg_int(a, "x", 0, 0, 1000000, &x) || arg_int(a, "y", 0, 0, 1000000, &y) || arg_int(a, "width", 0, 0, 1000000, &w)
        || arg_int(a, "height", 0, 0, 1000000, &h) || arg_bool(a, "grid", 0, &grid)) { fail(r, "%s", a->err); return; }
    int zi = arg(a, "zoom");
    if (zi >= 0 && a->t[zi].type != J_NULL && (json_num(a->s, a->t, zi, &zoom) ||
        (zoom != 0.5 && zoom != 1 && zoom != 2 && zoom != 3 && zoom != 4))) { fail(r, "zoom must be 0.5, 1, 2, 3 or 4"); return; }
    if (need_png(r)) return;
    int rc = cap_size(&sw, &sh);
    if (rc) { fail(r, "The screen can't be read: %s.", screen_error(rc)); return; }
    if (x >= sw || y >= sh) { fail(r, "(%d, %d) is off the screen (%d x %d).", x, y, sw, sh); return; }
    if (!w || x + w > sw) w = sw - x;
    if (!h || y + h > sh) h = sh - y;
    int z2 = (int)(zoom * 2);
    if ((long)w * z2 / 2 > MAX_SIDE || (long)h * z2 / 2 > MAX_SIDE) {
        fail(r, "That picture would be %ld x %ld: at most %d on a side. Use a smaller region or zoom.",
             (long)w * z2 / 2, (long)h * z2 / 2, MAX_SIDE);
        return;
    }
    struct frame f;
    if ((rc = grab(&f, 1))) { fail(r, "The screen can't be read: %s.", screen_error(rc)); return; }
    int ow, oh;
    if (add_image(r, &f, x, y, w, h, z2, grid, &ow, &oh)) fail(r, "The screenshot couldn't be encoded (out of memory?).");
    else if (x == 0 && y == 0 && w == sw && h == sh && z2 == 2)
        add_text(r, "The whole screen, %d x %d pixels.", sw, sh);
    else
        add_text(r, "Screen region x %d-%d, y %d-%d (of %d x %d) at zoom %g: a %d x %d picture. Coordinates to touch are "
                 "screen pixels: picture pixel (u, v) is screen point (%d + u / %g, %d + v / %g).",
                 x, x + w - 1, y, y + h - 1, sw, sh, zoom, ow, oh, x, zoom, y, zoom);
    if (grid) add_text(r, "Grid lines every %d screen pixels, labelled with their screen coordinate.", img_grid_step(z2));
    free(f.rgb);
}

static void t_tap(struct args *a, struct result *r, int kind)   // 0 tap, 1 double tap, 2 long press
{
    int x, y, hold, mode, w, h;
    if (arg_int(a, "x", REQUIRED, 0, 1000000, &x) || arg_int(a, "y", REQUIRED, 0, 1000000, &y)
        || (kind == 0 && arg_int(a, "hold_ms", 80, 10, 4000, &hold))
        || (kind == 2 && arg_int(a, "duration_ms", 1000, 300, 10000, &hold)) || arg_shot(a, 1, &mode)) { fail(r, "%s", a->err); return; }
    if (on_screen(r, x, y, &w, &h) || touch_ready(r)) return;
    struct frame before;
    grab(&before, 1);
    gesture_lock();
    if (kind == 1) {
        finger(0, x, y); sleep_ms(60); touch_up(); sleep_ms(90);
        finger(0, x, y); sleep_ms(60); touch_up();
    } else {
        finger(0, x, y); sleep_ms(hold); touch_up();
    }
    gesture_unlock();
    char did[96];
    snprintf(did, sizeof did, kind == 0 ? "Tapped (%d, %d)" : kind == 1 ? "Double-tapped (%d, %d)" : "Long-pressed (%d, %d)", x, y);
    report_change(r, &before, mode, did);
    free(before.rgb);
}

static void t_drag(struct args *a, struct result *r)
{
    int x0, y0, x1, y1, ms, hold, mode, w, h;
    if (arg_int(a, "from_x", REQUIRED, 0, 1000000, &x0) || arg_int(a, "from_y", REQUIRED, 0, 1000000, &y0)
        || arg_int(a, "to_x", REQUIRED, 0, 1000000, &x1) || arg_int(a, "to_y", REQUIRED, 0, 1000000, &y1)
        || arg_int(a, "duration_ms", 400, 50, 10000, &ms) || arg_int(a, "hold_ms", 100, 0, 3000, &hold)
        || arg_shot(a, 1, &mode)) { fail(r, "%s", a->err); return; }
    if (on_screen(r, x0, y0, &w, &h) || on_screen(r, x1, y1, &w, &h) || touch_ready(r)) return;
    struct frame before;
    grab(&before, 1);
    gesture_lock();
    finger(0, x0, y0);
    sleep_ms(hold);
    slide(x0, y0, x1, y1, ms);
    sleep_ms(50);
    touch_up();
    gesture_unlock();
    char did[96];
    snprintf(did, sizeof did, "Dragged from (%d, %d) to (%d, %d)", x0, y0, x1, y1);
    report_change(r, &before, mode, did);
    free(before.rgb);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void t_scroll(struct args *a, struct result *r)
{
    static const char *const dirs[] = { "up", "down", "left", "right", NULL };
    int x, y, amount, mode, w, h;
    char d[8];
    if (arg_int(a, "x", REQUIRED, 0, 1000000, &x) || arg_int(a, "y", REQUIRED, 0, 1000000, &y)
        || arg_str(a, "direction", 1, dirs, d, sizeof d) || arg_int(a, "amount", 300, 20, 2000, &amount)
        || arg_shot(a, 1, &mode)) { fail(r, "%s", a->err); return; }
    if (on_screen(r, x, y, &w, &h) || touch_ready(r)) return;
    // to see more below, the content moves up: the finger goes up. The stroke is centred on (x, y), kept on screen.
    int dx = !strcmp(d, "left") ? 1 : !strcmp(d, "right") ? -1 : 0, dy = !strcmp(d, "up") ? 1 : !strcmp(d, "down") ? -1 : 0;
    int x0 = clampi(x - dx * amount / 2, 0, w - 1), y0 = clampi(y - dy * amount / 2, 0, h - 1);
    int x1 = clampi(x0 + dx * amount, 0, w - 1), y1 = clampi(y0 + dy * amount, 0, h - 1);
    struct frame before;
    grab(&before, 1);
    gesture_lock();
    finger(0, x0, y0);
    sleep_ms(30);
    slide(x0, y0, x1, y1, 350);
    sleep_ms(150);                 // still before lifting: no fling
    touch_up();
    gesture_unlock();
    char did[128];
    snprintf(did, sizeof did, "Scrolled %s (the finger moved from (%d, %d) to (%d, %d))", d, x0, y0, x1, y1);
    report_change(r, &before, mode, did);
    free(before.rgb);
}

static void t_touch(struct args *a, struct result *r)
{
    static const char *const acts[] = { "down", "move", "up", NULL };
    char act[8];
    int x = -1, y = -1, mode, w, h;
    if (arg_str(a, "action", 1, acts, act, sizeof act) || arg_shot(a, 0, &mode)) { fail(r, "%s", a->err); return; }
    if (strcmp(act, "up")) {
        if (arg_int(a, "x", REQUIRED, 0, 1000000, &x) || arg_int(a, "y", REQUIRED, 0, 1000000, &y)) { fail(r, "%s", a->err); return; }
        if (on_screen(r, x, y, &w, &h)) return;
    }
    if (touch_ready(r)) return;
    gesture_lock();
    if (!strcmp(act, "down")) finger(0, x, y);
    else if (!strcmp(act, "move")) finger(1, x, y);
    else touch_up();
    gesture_unlock();
    if (strcmp(act, "up")) add_text(r, "Finger %s at (%d, %d)%s.", !strcmp(act, "down") ? "down" : "moved", x, y,
                                    " (lift it with action=up when done)");
    else add_text(r, "Finger lifted.");
    if (mode) {
        sleep_ms(150);
        struct frame f;
        if (grab(&f, 1) == 0) { shot_full(r, &f, mode); free(f.rgb); }
    }
}

static void t_wait(struct args *a, struct result *r)
{
    static const char *const untils[] = { "change", "stable", NULL };
    char until[8];
    int timeout, mode;
    if (arg_str(a, "until", 0, untils, until, sizeof until) || arg_int(a, "timeout_ms", 3000, 100, 20000, &timeout)
        || arg_shot(a, 1, &mode)) { fail(r, "%s", a->err); return; }
    int stable = !strcmp(until, "stable");
    long long t0 = now_ms();
    struct frame first, prev, cur = { 0 };
    int rc = grab(&first, 1);
    if (rc) { fail(r, "The screen can't be read: %s.", screen_error(rc)); return; }
    prev = first;
    int done = 0;
    while (!done && now_ms() - t0 < timeout) {
        sleep_ms(stable ? 150 : 100);
        if (grab(&cur, 1)) break;
        done = stable ? same(&prev, &cur) : !same(&first, &cur);
        if (prev.rgb != first.rgb) free(prev.rgb);
        prev = cur;
        cur.rgb = NULL;
    }
    int took = (int)(now_ms() - t0);
    if (stable) add_text(r, done ? "The screen is still (after %d ms)." : "The screen was still changing after %d ms.", took);
    else if (done) {
        int box[4];
        diff_box(&first, &prev, box);
        add_text(r, "The screen changed after %d ms, in x %d-%d, y %d-%d.", took, box[0], box[2], box[1], box[3]);
    } else add_text(r, "The screen did not change in %d ms.", took);
    shot_full(r, &prev, mode);
    if (prev.rgb != first.rgb) free(prev.rgb);
    free(first.rgb);
}

static void t_info(struct result *r)
{
    int w = 0, h = 0, rc = cap_size(&w, &h);
    add_text(r, "{\"width\":%d,\"height\":%d,\"display\":\"%s\",\"pixel_format\":\"%s\",\"touchscreen\":\"%s\","
             "\"png\":%s,\"addin_version\":\"%s\"}\nCoordinates for every touch tool: x 0-%d, y 0-%d, origin top left.",
             w, h, rc ? cap_error(rc) : "ok", cap_format(), touch_device(), png_init() ? "false" : "true", REMOTE_VERSION,
             w - 1, h - 1);
}

static void describe(struct sb *b, const struct midi_msg *m)
{
    char n[8];
    int st = m->b[0] & 0xF0, ch = (m->b[0] & 15) + 1;
    sb_printf(b, "%6d ms  ", m->ms);
    switch (st) {
    case 0x90: sb_printf(b, "note_on  ch %d  %s (%d)  velocity %d\n", ch, note_name(m->b[1], n), m->b[1], m->b[2]); break;
    case 0x80: sb_printf(b, "note_off ch %d  %s (%d)\n", ch, note_name(m->b[1], n), m->b[1]); break;
    case 0xA0: sb_printf(b, "poly_pressure ch %d  %s (%d)  %d\n", ch, note_name(m->b[1], n), m->b[1], m->b[2]); break;
    case 0xB0: sb_printf(b, "cc ch %d  controller %d  value %d\n", ch, m->b[1], m->b[2]); break;
    case 0xC0: sb_printf(b, "program_change ch %d  %d\n", ch, m->b[1]); break;
    case 0xD0: sb_printf(b, "channel_pressure ch %d  %d\n", ch, m->b[1]); break;
    case 0xE0: sb_printf(b, "pitch_bend ch %d  %d\n", ch, (m->b[2] << 7 | m->b[1]) - 8192); break;
    default:
        if (m->b[0] == 0xF0) sb_printf(b, "sysex (%d bytes)\n", m->len);
        else sb_printf(b, "%s\n", m->b[0] == 0xFA ? "start" : m->b[0] == 0xFB ? "continue" : m->b[0] == 0xFC ? "stop" : "clock");
    }
}

#define MAX_HEARD 400
static void listen_report(struct result *r, int ms)
{
    struct midi_msg *m = malloc(MAX_HEARD * sizeof *m);
    if (!m) return;
    int n = midi_listen(ms, m, MAX_HEARD), clocks = 0;
    struct sb b = { 0 };
    sb_printf(&b, "Heard on MPC Remote In in %d ms: %d message(s)%s\n", ms, n, n ? ":" : " (nothing is connected to it, or nothing was sent)");
    for (int i = 0; i < n; i++) {
        if (m[i].b[0] == 0xF8) { clocks++; continue; }      // clock ticks are counted, not listed
        describe(&b, &m[i]);
    }
    if (clocks) sb_printf(&b, "(and %d MIDI clock ticks)\n", clocks);
    add_sb_text(r, &b);
    sb_free(&b);
    free(m);
}

static int midi_ready(struct result *r)
{
    if (midi_open() == 0) return 0;
    fail(r, "MIDI isn't available: %s.", midi_error());
    return -1;
}

static void t_play(struct args *a, struct result *r)
{
    static const char *const modes[] = { "together", "one_by_one", NULL };
    char mode[12];
    int vel, dur, gap, ch, listen, shot, notes[64], n = 0;
    if (arg_str(a, "mode", 0, modes, mode, sizeof mode) || arg_int(a, "velocity", 100, 1, 127, &vel)
        || arg_int(a, "duration_ms", 400, 10, 10000, &dur) || arg_int(a, "gap_ms", 50, 0, 5000, &gap)
        || arg_int(a, "channel", 1, 1, 16, &ch) || arg_int(a, "listen_ms", 0, 0, 10000, &listen)
        || arg_shot(a, 0, &shot)) { fail(r, "%s", a->err); return; }
    int ni = arg(a, "notes");
    if (ni < 0 || (a->t[ni].type != J_ARR && a->t[ni].type != J_STR && a->t[ni].type != J_NUM)) { fail(r, "notes is required: a list such as [\"C3\", \"E3\", \"G3\"] or [60, 64, 67]"); return; }
    if (a->t[ni].type == J_ARR) {
        if (a->t[ni].size < 1 || a->t[ni].size > 64) { fail(r, "notes must hold 1 to 64 notes"); return; }
        for (int i = ni + 1, k = 0; k < a->t[ni].size; k++, i = a->t[i].skip) {
            char what[24];
            snprintf(what, sizeof what, "notes[%d]", k);
            if (arg_note(a, i, what, &notes[n++])) { fail(r, "%s", a->err); return; }
        }
    } else if (arg_note(a, ni, "notes", &notes[n++])) { fail(r, "%s", a->err); return; }
    int one = !strcmp(mode, "one_by_one");
    long long total = one ? (long long)n * dur + (long long)(n - 1) * gap : dur;
    if (total > 30000) { fail(r, "That would take %lld ms: at most 30000.", total); return; }
    if (midi_ready(r)) return;
    if (listen) midi_drain();
    uint8_t on = (uint8_t)(0x90 | (ch - 1)), off = (uint8_t)(0x80 | (ch - 1));
    int bad = 0;
    if (!one) {
        for (int i = 0; i < n; i++) bad |= midi_send((uint8_t[]){ on, (uint8_t)notes[i], (uint8_t)vel }, 3);
        sleep_ms(dur);
        for (int i = 0; i < n; i++) bad |= midi_send((uint8_t[]){ off, (uint8_t)notes[i], 0 }, 3);
    } else {
        for (int i = 0; i < n; i++) {
            bad |= midi_send((uint8_t[]){ on, (uint8_t)notes[i], (uint8_t)vel }, 3);
            sleep_ms(dur);
            bad |= midi_send((uint8_t[]){ off, (uint8_t)notes[i], 0 }, 3);
            if (i < n - 1) sleep_ms(gap);
        }
    }
    struct sb b = { 0 };
    char nm[8];
    sb_printf(&b, "%s %s on channel %d, velocity %d, %d ms%s:", bad ? "Tried to play" : "Played",
              one ? "one by one" : n > 1 ? "together" : "", ch, vel, dur, one ? " each" : "");
    for (int i = 0; i < n; i++) sb_printf(&b, " %s (%d)", note_name(notes[i], nm), notes[i]);
    sb_puts(&b, bad ? ". Some sends failed." : ". If MPC didn't play them, see midi_status (and Preferences > MIDI).");
    add_sb_text(r, &b);
    sb_free(&b);
    if (listen) listen_report(r, listen);
    if (shot) {
        struct frame f;
        if (grab(&f, 1) == 0) { shot_full(r, &f, shot); free(f.rgb); }
    }
}

static int hexbytes(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (*s) {
        if (isspace((unsigned char)*s) || *s == ',') { s++; continue; }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        int hi = isxdigit((unsigned char)s[0]) ? (isdigit((unsigned char)s[0]) ? s[0] - '0' : (tolower(s[0]) - 'a' + 10)) : -1;
        int lo = s[0] && isxdigit((unsigned char)s[1]) ? (isdigit((unsigned char)s[1]) ? s[1] - '0' : (tolower(s[1]) - 'a' + 10)) : -1;
        if (hi < 0 || lo < 0 || n >= max) return -1;
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    return n;
}

static void t_send(struct args *a, struct result *r)
{
    static const char *const types[] = { "note_on", "note_off", "cc", "program_change", "pitch_bend", "channel_pressure",
                                         "poly_pressure", "start", "stop", "continue", "clock", "mmc", "sysex", NULL };
    static const char *const mmcs[] = { "stop", "play", "deferred_play", "fast_forward", "rewind", "record_strobe",
                                        "record_exit", "pause", "locate_start", NULL };
    static const uint8_t mmc_code[] = { 1, 2, 3, 4, 5, 6, 7, 9, 0x44 };
    char type[20], cmd[16], hex[1024];
    int ch, listen, v, note = 0;
    uint8_t m[300];
    int n = 0;
    if (arg_str(a, "type", 1, types, type, sizeof type) || arg_int(a, "channel", 1, 1, 16, &ch)
        || arg_int(a, "listen_ms", 0, 0, 10000, &listen)) { fail(r, "%s", a->err); return; }
    uint8_t c = (uint8_t)(ch - 1);
    int is_note = !strcmp(type, "note_on") || !strcmp(type, "note_off") || !strcmp(type, "poly_pressure");
    if (is_note && arg_note(a, arg(a, "note"), "note", &note)) { fail(r, "%s", a->err); return; }
    if (!strcmp(type, "note_on") || !strcmp(type, "note_off")) {
        if (arg_int(a, "velocity", !strcmp(type, "note_on") ? 100 : 0, 0, 127, &v)) { fail(r, "%s", a->err); return; }
        m[0] = (uint8_t)((type[6] == 'n' ? 0x90 : 0x80) | c); m[1] = (uint8_t)note; m[2] = (uint8_t)v; n = 3;
    } else if (!strcmp(type, "poly_pressure")) {
        if (arg_int(a, "value", REQUIRED, 0, 127, &v)) { fail(r, "%s", a->err); return; }
        m[0] = 0xA0 | c; m[1] = (uint8_t)note; m[2] = (uint8_t)v; n = 3;
    } else if (!strcmp(type, "cc")) {
        int ctl;
        if (arg_int(a, "controller", REQUIRED, 0, 127, &ctl) || arg_int(a, "value", REQUIRED, 0, 127, &v)) { fail(r, "%s", a->err); return; }
        m[0] = 0xB0 | c; m[1] = (uint8_t)ctl; m[2] = (uint8_t)v; n = 3;
    } else if (!strcmp(type, "program_change") || !strcmp(type, "channel_pressure")) {
        if (arg_int(a, "value", REQUIRED, 0, 127, &v)) { fail(r, "%s", a->err); return; }
        m[0] = (uint8_t)((type[0] == 'p' ? 0xC0 : 0xD0) | c); m[1] = (uint8_t)v; n = 2;
    } else if (!strcmp(type, "pitch_bend")) {
        if (arg_int(a, "bend", 0, -8192, 8191, &v)) { fail(r, "%s", a->err); return; }
        v += 8192;
        m[0] = 0xE0 | c; m[1] = (uint8_t)(v & 127); m[2] = (uint8_t)(v >> 7); n = 3;
    } else if (!strcmp(type, "start")) { m[0] = 0xFA; n = 1; }
    else if (!strcmp(type, "stop")) { m[0] = 0xFC; n = 1; }
    else if (!strcmp(type, "continue")) { m[0] = 0xFB; n = 1; }
    else if (!strcmp(type, "clock")) { m[0] = 0xF8; n = 1; }
    else if (!strcmp(type, "mmc")) {
        if (arg_str(a, "mmc", 1, mmcs, cmd, sizeof cmd)) { fail(r, "%s", a->err); return; }
        int k = 0;
        while (strcmp(mmcs[k], cmd)) k++;
        uint8_t head[] = { 0xF0, 0x7F, 0x7F, 0x06, mmc_code[k] };   // to every device
        memcpy(m, head, sizeof head);
        n = sizeof head;
        if (mmc_code[k] == 0x44) {                                   // locate to 00:00:00:00.00
            uint8_t loc[] = { 0x06, 0x01, 0, 0, 0, 0, 0 };
            memcpy(m + n, loc, sizeof loc);
            n += (int)sizeof loc;
        }
        m[n++] = 0xF7;
    } else {
        if (arg_str(a, "sysex", 1, NULL, hex, sizeof hex)) { fail(r, "%s", a->err); return; }
        n = hexbytes(hex, m, (int)sizeof m);
        if (n < 3 || m[0] != 0xF0 || m[n - 1] != 0xF7) { fail(r, "sysex must be hex bytes from F0 to F7, at most %zu", sizeof m); return; }
        for (int i = 1; i < n - 1; i++) if (m[i] & 0x80) { fail(r, "sysex data bytes must be 00 to 7F (byte %d is %02X)", i, m[i]); return; }
    }
    if (midi_ready(r)) return;
    if (listen) midi_drain();
    if (midi_send(m, (size_t)n)) { fail(r, "The message couldn't be sent."); return; }
    struct sb b = { 0 };
    sb_printf(&b, "Sent %s:", type);
    for (int i = 0; i < n; i++) sb_printf(&b, " %02X", m[i]);
    add_sb_text(r, &b);
    sb_free(&b);
    if (listen) listen_report(r, listen);
}

static void t_listen(struct args *a, struct result *r)
{
    int ms;
    if (arg_int(a, "duration_ms", 3000, 100, 30000, &ms)) { fail(r, "%s", a->err); return; }
    if (midi_ready(r)) return;
    midi_drain();
    listen_report(r, ms);
}

static void t_midi_status(struct result *r)
{
    struct sb b = { 0 };
    if (midi_open()) sb_printf(&b, "MIDI isn't available: %s.\n", midi_error());
    else sb_printf(&b, "The addin is sequencer client %d, \"MPC Remote\": port 0 \"Out\" plays into MPC, port 1 \"In\" "
                       "hears what is sent to it. \"Connecting To\" under Out lists who receives it: MPC appears there "
                       "once it has picked the port up.\n\n", midi_client());
    FILE *f = fopen("/proc/asound/seq/clients", "re");
    if (!f) sb_puts(&b, "(/proc/asound/seq/clients can't be read)\n");
    else {
        char line[256];
        int keep = 0;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "Client ", 7)) keep = strncmp(line, "Client info", 11) != 0;
            if (keep && (!strncmp(line, "Client ", 7) || !strncmp(line, "  Port ", 7) || !strncmp(line, "    Conn", 8)))
                sb_puts(&b, line);       // clients, ports and connections; not the memory pool figures
        }
        fclose(f);
    }
    add_sb_text(r, &b);
    sb_free(&b);
}

static void t_status(struct result *r)
{
    struct sb b = { 0 };
    dev_status(&b);
    add_sb_text(r, &b);
    sb_free(&b);
}

static void t_files(struct args *a, struct result *r, int what)   // 0 list, 1 find, 2 read
{
    char path[1024], pat[256], err[512];
    int max, off, maxb;
    struct sb b = { 0 };
    if (arg_str(a, "path", what == 2, NULL, path, sizeof path)) { fail(r, "%s", a->err); return; }
    if (what == 0) {
        if (!path[0]) { sb_puts(&b, "Folders files can be read from:\n"); files_roots(&b); }
        else if (files_list(path, &b, err, sizeof err)) { fail(r, "%s", err); sb_free(&b); return; }
    } else if (what == 1) {
        if (arg_str(a, "pattern", 1, NULL, pat, sizeof pat) || arg_int(a, "max_results", 100, 1, 500, &max)) { fail(r, "%s", a->err); return; }
        if (path[0]) {
            if (files_find(path, pat, max, &b, err, sizeof err)) { fail(r, "%s", err); sb_free(&b); return; }
        } else {                                    // every root
            struct sb roots = { 0 };
            files_roots(&roots);
            for (char *line = roots.p; line && *line;) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                if (line[0] == '/' && !strstr(line, "(not present)")) {
                    char root[256];
                    snprintf(root, sizeof root, "%s", line);
                    files_find(root, pat, max, &b, err, sizeof err);
                }
                line = nl ? nl + 1 : NULL;
            }
            if (!b.n) sb_puts(&b, "File access is off (mcp_files=none), or no readable folder is present.");
            sb_free(&roots);
        }
    } else {
        if (arg_int(a, "offset", 0, 0, 2147483647, &off) || arg_int(a, "max_bytes", 16384, 256, 65536, &maxb)) { fail(r, "%s", a->err); return; }
        if (files_read(path, off, maxb, &b, err, sizeof err)) { fail(r, "%s", err); sb_free(&b); return; }
    }
    add_sb_text(r, &b);
    sb_free(&b);
}

// ---- JSON-RPC ----
static void reply_head(struct sb *o, const char *s, const struct jtok *t, int id)
{
    sb_puts(o, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id < 0) sb_puts(o, "null");
    else if (t[id].type == J_STR) sb_raw(o, s + t[id].start - 1, (size_t)(t[id].end - t[id].start + 2));
    else sb_raw(o, s + t[id].start, (size_t)(t[id].end - t[id].start));
}

static void rpc_error(struct sb *o, const char *s, const struct jtok *t, int id, int code, const char *msg)
{
    reply_head(o, s, t, id);
    sb_printf(o, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_jstr(o, msg);
    sb_puts(o, "}}");
}

static void initialize(struct sb *o, const char *s, const struct jtok *t, int id, int params)
{
    char want[32] = "";
    json_str(s, t, json_get(s, t, params, "protocolVersion"), want, sizeof want);
    const char *ver = VERSIONS[0];
    for (unsigned i = 0; i < sizeof VERSIONS / sizeof *VERSIONS; i++) if (!strcmp(want, VERSIONS[i])) ver = VERSIONS[i];
    reply_head(o, s, t, id);
    sb_puts(o, ",\"result\":{\"protocolVersion\":");
    sb_jstr(o, ver);
    sb_puts(o, ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"mpc-remote\","
               "\"title\":\"MPC Remote\",\"version\":\"" REMOTE_VERSION "\"},\"instructions\":");
    sb_jstr(o, INSTRUCTIONS);
    sb_puts(o, "}}");
}

static void call_tool(struct sb *o, const char *s, const struct jtok *t, int id, int params)
{
    char name[48];
    if (json_str(s, t, json_get(s, t, params, "name"), name, sizeof name)) {
        rpc_error(o, s, t, id, -32602, "tools/call needs a tool name");
        return;
    }
    struct args a = { .s = s, .t = t, .obj = json_get(s, t, params, "arguments") };
    if (a.obj >= 0 && t[a.obj].type == J_NULL) a.obj = -1;
    if (a.obj >= 0 && t[a.obj].type != J_OBJ) { rpc_error(o, s, t, id, -32602, "arguments must be an object"); return; }
    struct result r = { 0 };
    if (!strcmp(name, "screenshot")) t_screenshot(&a, &r);
    else if (!strcmp(name, "tap")) t_tap(&a, &r, 0);
    else if (!strcmp(name, "double_tap")) t_tap(&a, &r, 1);
    else if (!strcmp(name, "long_press")) t_tap(&a, &r, 2);
    else if (!strcmp(name, "drag")) t_drag(&a, &r);
    else if (!strcmp(name, "scroll")) t_scroll(&a, &r);
    else if (!strcmp(name, "touch")) t_touch(&a, &r);
    else if (!strcmp(name, "wait_for_screen")) t_wait(&a, &r);
    else if (!strcmp(name, "get_screen_info")) t_info(&r);
    else if (!strcmp(name, "play_notes")) t_play(&a, &r);
    else if (!strcmp(name, "send_midi")) t_send(&a, &r);
    else if (!strcmp(name, "midi_listen")) t_listen(&a, &r);
    else if (!strcmp(name, "midi_status")) t_midi_status(&r);
    else if (!strcmp(name, "device_status")) t_status(&r);
    else if (!strcmp(name, "list_files")) t_files(&a, &r, 0);
    else if (!strcmp(name, "find_files")) t_files(&a, &r, 1);
    else if (!strcmp(name, "read_text_file")) t_files(&a, &r, 2);
    else {
        char m[96];
        snprintf(m, sizeof m, "Unknown tool: %s", name);
        rpc_error(o, s, t, id, -32602, m);
        sb_free(&r.b);
        return;
    }
    if (r.b.oom) { sb_free(&r.b); r = (struct result){ 0 }; fail(&r, "Out of memory building the reply."); }
    reply_head(o, s, t, id);
    sb_puts(o, ",\"result\":{\"content\":[");
    if (r.b.n) sb_raw(o, r.b.p, r.b.n);
    sb_printf(o, "],\"isError\":%s}}", r.error ? "true" : "false");
    sb_free(&r.b);
}

int mcp_handle(const char *body, size_t n, char **out, size_t *outlen)
{
    struct sb o = { 0 };
    struct jtok *t = malloc(MAX_TOKENS * sizeof *t);   // 10 KB: kept off the 256 KB thread stack
    if (!t) return 500;
    int status = 200;
    *out = NULL;
    *outlen = 0;
    int k = json_parse(body, n, t, MAX_TOKENS);
    if (k < 0) {
        rpc_error(&o, body, t, -1, -32700, "Parse error: the body isn't JSON (or is too large or deep)");
        status = 400;
    } else if (t[0].type == J_ARR) {
        rpc_error(&o, body, t, -1, -32600, "Batches aren't supported: send one message per request");
        status = 400;
    } else if (t[0].type != J_OBJ) {
        rpc_error(&o, body, t, -1, -32600, "Invalid request: not a JSON-RPC object");
        status = 400;
    } else {
        int id = json_get(body, t, 0, "id"), mi = json_get(body, t, 0, "method"), params = json_get(body, t, 0, "params");
        char method[64];
        if (id >= 0 && t[id].type != J_STR && t[id].type != J_NUM) id = -1;
        if (json_str(body, t, mi, method, sizeof method)) method[0] = 0;
        if (mi < 0 || id < 0) {      // a response (the server sends no requests) or a notification (initialized, cancelled ...)
            free(t);
            return 202;
        }
        if (!strcmp(method, "initialize")) initialize(&o, body, t, id, params);
        else if (!strcmp(method, "ping")) { reply_head(&o, body, t, id); sb_puts(&o, ",\"result\":{}}"); }
        else if (!strcmp(method, "tools/list")) {
            reply_head(&o, body, t, id);
            sb_puts(&o, ",\"result\":{\"tools\":");
            sb_puts(&o, TOOLS_JSON);
            sb_puts(&o, "}}");
        } else if (!strcmp(method, "tools/call")) call_tool(&o, body, t, id, params);
        else if (!strcmp(method, "resources/list")) { reply_head(&o, body, t, id); sb_puts(&o, ",\"result\":{\"resources\":[]}}"); }
        else if (!strcmp(method, "prompts/list")) { reply_head(&o, body, t, id); sb_puts(&o, ",\"result\":{\"prompts\":[]}}"); }
        else {
            char m[96];
            snprintf(m, sizeof m, "Method not found: %s", method);
            rpc_error(&o, body, t, id, -32601, m);
        }
    }
    free(t);
    if (o.oom) { sb_free(&o); return 500; }
    *out = o.p;
    *outlen = o.n;
    return status;
}
