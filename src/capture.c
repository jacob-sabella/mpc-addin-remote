// Screen capture through DRM/KMS. A process can't read another process's buffer object, but a root process
// (CAP_SYS_ADMIN) gets a GEM handle for any framebuffer from GETFB2/GETFB on its own file description, maps it
// with MAP_DUMB, and so reads exactly what is on screen: no /dev/mem, no hard-coded size or address. The CRTC is
// asked for its framebuffer on every frame, so a double-buffered display is followed through page flips.
#define _GNU_SOURCE
#include "capture.h"
#include "drm_min.h"
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { E_NODISPLAY = -1, E_NOHANDLE = -2, E_FORMAT = -3, E_MAP = -4, E_SMALL = -5, E_GONE = -6 };

#define NMAPS 4              // framebuffers kept mapped (double or triple buffering)
#define REMAP_NS 2000000000LL // mappings are dropped this often: a framebuffer id can be reused for a new buffer

struct fbmap {
    uint32_t fb_id, w, h, pitch, fmt;
    uint8_t *base;           // the mapping; pixels start at base + off
    size_t len, off;
};

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int drm_fd = -1;
static uint32_t crtc_id;
static struct fbmap maps[NMAPS];
static int nmaps;
static long long mapped_at;
static uint32_t last_fmt;
static uint8_t *row;         // one source row, copied out of the (uncached) buffer in one go
static size_t rowsz;
static int rotation = -1;    // quarter turn to upright, -1 auto
static int last_ms;          // the last cap_frame()'s time

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void unmap_all(void)
{
    for (int i = 0; i < nmaps; i++) munmap(maps[i].base, maps[i].len);
    nmaps = 0;
}

static int bytes_per_pixel(uint32_t fmt)
{
    switch (fmt) {
    case FMT_XRGB8888: case FMT_ARGB8888: case FMT_XBGR8888: case FMT_ABGR8888: return 4;
    case FMT_RGB565: return 2;
    default: return 0;
    }
}

#ifdef REMOTE_TEST
// Test builds only: REMOTE_FAKE_FB="file,width,height" stands in for the display (XRGB8888, stride = width * 4).
static int fake_open(struct fbmap *m)
{
    const char *spec = getenv("REMOTE_FAKE_FB");
    if (!spec) return 0;
    char path[256];
    unsigned w, h;
    if (sscanf(spec, "%255[^,],%u,%u", path, &w, &h) != 3 || !w || !h || w > 8192 || h > 8192) return E_NODISPLAY;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return E_NODISPLAY;
    size_t len = (size_t)w * 4 * h;
    void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return E_MAP;
    *m = (struct fbmap){ .fb_id = 1, .w = w, .h = h, .pitch = w * 4, .fmt = FMT_XRGB8888, .base = p, .len = len };
    return 1;
}
#endif

// Find a CRTC that is lit (has a mode and a framebuffer) on any DRM card.
static int open_display(void)
{
    for (int i = 0; i < 8; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/dri/card%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct drm_mode_card_res r = { 0 };
        uint32_t ids[32];
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == 0 && r.count_crtcs) {
            uint32_t n = r.count_crtcs < 32 ? r.count_crtcs : 32;
            memset(&r, 0, sizeof r);
            r.count_crtcs = n;
            r.crtc_id_ptr = (uint64_t)(uintptr_t)ids;
            if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == 0) {
                if (r.count_crtcs < n) n = r.count_crtcs;
                for (uint32_t k = 0; k < n; k++) {
                    struct drm_mode_crtc c = { .crtc_id = ids[k] };
                    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) == 0 && c.fb_id && c.mode_valid) {
                        drm_fd = fd;
                        crtc_id = ids[k];
                        return 0;
                    }
                }
            }
        }
        close(fd);
    }
    return E_NODISPLAY;
}

static int map_fb(uint32_t fb_id, struct fbmap *m)
{
    uint32_t w, h, pitch, fmt, handle, off = 0;
    struct drm_mode_fb_cmd2 f2 = { .fb_id = fb_id };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETFB2, &f2) == 0) {
        if ((f2.flags & DRM_MODE_FB_MODIFIERS) && f2.modifier[0] != DRM_FORMAT_MOD_LINEAR) {
            for (int i = 0; i < 4; i++)   // GETFB2 gives one handle per unique buffer object
                if (f2.handles[i] && (i == 0 || f2.handles[i] != f2.handles[0])) {
                    struct drm_gem_close gc = { .handle = f2.handles[i] };
                    ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
                }
            return E_FORMAT;               // tiled or compressed: not readable as plain rows
        }
        w = f2.width; h = f2.height; fmt = f2.pixel_format;
        handle = f2.handles[0]; pitch = f2.pitches[0]; off = f2.offsets[0];
        for (int i = 1; i < 4; i++)
            if (f2.handles[i] && f2.handles[i] != handle) {
                struct drm_gem_close gc = { .handle = f2.handles[i] };
                ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
            }
    } else {
        struct drm_mode_fb_cmd f = { .fb_id = fb_id };   // kernels before 5.7
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETFB, &f)) return E_GONE;
        w = f.width; h = f.height; pitch = f.pitch; handle = f.handle;
        fmt = f.bpp == 32 ? FMT_XRGB8888 : f.bpp == 16 ? FMT_RGB565 : 0;
    }
    if (!handle) return E_NOHANDLE;        // not root: GETFB hands out handles only with CAP_SYS_ADMIN
    int rc = 0;
    int bpp = bytes_per_pixel(fmt);
    if (!bpp || !w || !h || pitch < w * (uint32_t)bpp) rc = E_FORMAT;
    struct drm_mode_map_dumb md = { .handle = handle };
    if (!rc && ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) rc = E_MAP;
    void *p = MAP_FAILED;
    size_t len = (size_t)off + (size_t)pitch * h;
    if (!rc) {
        p = mmap(NULL, len, PROT_READ, MAP_SHARED, drm_fd, (off_t)md.offset);
        if (p == MAP_FAILED) rc = E_MAP;
    }
    struct drm_gem_close gc = { .handle = handle };   // the mapping keeps the buffer alive
    ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc);
    if (rc) return rc;
    *m = (struct fbmap){ .fb_id = fb_id, .w = w, .h = h, .pitch = pitch, .fmt = fmt, .base = p, .len = len, .off = off };
    return 0;
}

// The framebuffer on screen now, mapped. Called with mtx held.
static int current(struct fbmap **out)
{
#ifdef REMOTE_TEST
    if (getenv("REMOTE_FAKE_FB")) {
        if (!nmaps) {
            int r = fake_open(&maps[0]);
            if (r <= 0) return r ? r : E_NODISPLAY;
            nmaps = 1;
        }
        *out = &maps[0];
        last_fmt = maps[0].fmt;
        return 0;
    }
#endif
    if (drm_fd < 0) {
        int r = open_display();
        if (r) return r;
    }
    struct drm_mode_crtc c = { .crtc_id = crtc_id };
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &c) || !c.fb_id || !c.mode_valid) {
        unmap_all();                       // the display went away (or another CRTC took over): look again
        close(drm_fd);
        drm_fd = -1;
        return E_NODISPLAY;
    }
    long long t = now_ns();
    if (t - mapped_at > REMAP_NS) { unmap_all(); mapped_at = t; }
    for (int i = 0; i < nmaps; i++)
        if (maps[i].fb_id == c.fb_id) { *out = &maps[i]; last_fmt = maps[i].fmt; return 0; }
    if (nmaps == NMAPS) {                  // more buffers than expected: drop the oldest
        munmap(maps[0].base, maps[0].len);
        memmove(&maps[0], &maps[1], sizeof maps[0] * (NMAPS - 1));
        nmaps--;
    }
    int r = map_fb(c.fb_id, &maps[nmaps]);
    if (r) return r;
    *out = &maps[nmaps++];
    last_fmt = (*out)->fmt;
    return 0;
}

void cap_set_rotation(int degrees)
{
    pthread_mutex_lock(&mtx);
    rotation = degrees < 0 ? -1 : (degrees % 360) / 90 * 90;
    pthread_mutex_unlock(&mtx);
}

// The quarter turn that makes a framebuffer upright: as set, or with auto a portrait scanout turns 90 degrees
// (MPC's UI is landscape; a portrait panel shows it rotated). Called with mtx held.
static int turn(const struct fbmap *m)
{
    return rotation >= 0 ? rotation : m->h > m->w ? 90 : 0;
}

int cap_size(int *w, int *h)
{
    pthread_mutex_lock(&mtx);
    struct fbmap *m;
    int r = current(&m);
    if (!r) {
        int t = turn(m);
        *w = (int)(t % 180 ? m->h : m->w);
        *h = (int)(t % 180 ? m->w : m->h);
    }
    pthread_mutex_unlock(&mtx);
    return r;
}

int cap_to_scanout(int x, int y, int *sx, int *sy, int *sw, int *sh)
{
    pthread_mutex_lock(&mtx);
    struct fbmap *m;
    int r = current(&m);
    if (!r) {
        int W = (int)m->w, H = (int)m->h, t = turn(m);
        int uw = t % 180 ? H : W, uh = t % 180 ? W : H;   // the upright size
        x = x < 0 ? 0 : x >= uw ? uw - 1 : x;
        y = y < 0 ? 0 : y >= uh ? uh - 1 : y;
        switch (t) {                                      // the inverse of the turn in cap_frame()
        case 90:  *sx = y;         *sy = H - 1 - x; break;
        case 180: *sx = W - 1 - x; *sy = H - 1 - y; break;
        case 270: *sx = W - 1 - y; *sy = x;         break;
        default:  *sx = x;         *sy = y;         break;
        }
        *sw = W;
        *sh = H;
    }
    pthread_mutex_unlock(&mtx);
    return r;
}

int cap_frame(uint8_t *out, size_t outsz, int scale, int *ow, int *oh)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    long long t0 = now_ns();
    pthread_mutex_lock(&mtx);
    struct fbmap *m;
    int r = current(&m);
    if (r) { pthread_mutex_unlock(&mtx); return r; }
    int W = (int)m->w / scale, H = (int)m->h / scale, bpp = bytes_per_pixel(m->fmt), t = turn(m);
    if ((size_t)W * H * 3 > outsz) { pthread_mutex_unlock(&mtx); return E_SMALL; }
    size_t need = (size_t)m->w * bpp;
    if (rowsz < need) {
        uint8_t *nr = realloc(row, need);
        if (!nr) { pthread_mutex_unlock(&mtx); return E_SMALL; }
        row = nr;
        rowsz = need;
    }
    int OW = t % 180 ? H : W;                             // the upright frame's width
    for (int y = 0; y < H; y++) {
        // source pixel (x, y) lands at out + d0 + x * dx: the scanout read row by row (fast on uncached memory),
        // the turn done while writing into the (cached) output
        ptrdiff_t d0, dx;
        switch (t) {
        case 90:  d0 = (ptrdiff_t)(H - 1 - y) * 3;                       dx = (ptrdiff_t)OW * 3;  break;
        case 180: d0 = ((ptrdiff_t)(H - 1 - y) * OW + (W - 1)) * 3;      dx = -3;                 break;
        case 270: d0 = ((ptrdiff_t)(W - 1) * OW + y) * 3;                dx = -(ptrdiff_t)OW * 3; break;
        default:  d0 = (ptrdiff_t)y * OW * 3;                            dx = 3;                  break;
        }
        memcpy(row, m->base + m->off + (size_t)y * scale * m->pitch, need);
        const uint8_t *s = row;
        uint8_t *o = out + d0;
        size_t step = (size_t)scale * bpp;
        switch (m->fmt) {
        case FMT_XRGB8888: case FMT_ARGB8888:          // little-endian: B G R X in memory
            for (int x = 0; x < W; x++, s += step, o += dx) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; }
            break;
        case FMT_XBGR8888: case FMT_ABGR8888:          // R G B X
            for (int x = 0; x < W; x++, s += step, o += dx) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; }
            break;
        default: {                                     // RGB565, little-endian
            for (int x = 0; x < W; x++, s += step, o += dx) {
                unsigned v = s[0] | (unsigned)s[1] << 8;
                o[0] = (uint8_t)((v >> 11 & 31) * 255 / 31);
                o[1] = (uint8_t)((v >> 5 & 63) * 255 / 63);
                o[2] = (uint8_t)((v & 31) * 255 / 31);
            }
        }
        }
    }
    last_ms = (int)((now_ns() - t0) / 1000000);
    pthread_mutex_unlock(&mtx);
    *ow = OW;
    *oh = t % 180 ? W : H;
    return 0;
}

int cap_last_ms(void)
{
    pthread_mutex_lock(&mtx);
    int v = last_ms;
    pthread_mutex_unlock(&mtx);
    return v;
}

const char *cap_format(void)
{
    static char s[5];
    pthread_mutex_lock(&mtx);
    uint32_t f = last_fmt;
    pthread_mutex_unlock(&mtx);
    if (!f) return "none";
    for (int i = 0; i < 4; i++) s[i] = (char)(f >> (8 * i));   // the same 4 bytes for any caller: a benign rewrite
    s[4] = 0;
    return s;
}

const char *cap_error(int rc)
{
    switch (rc) {
    case E_NODISPLAY: return "no lit display found under /dev/dri";
    case E_NOHANDLE: return "the framebuffer can't be read (needs root)";
    case E_FORMAT: return "unsupported framebuffer format (tiled, or not RGB)";
    case E_MAP: return "the framebuffer can't be mapped";
    case E_SMALL: return "out of memory";
    case E_GONE: return "the framebuffer went away";
    default: return "capture failed";
    }
}
