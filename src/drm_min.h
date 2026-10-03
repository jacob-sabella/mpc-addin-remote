// The few DRM/KMS ioctls the addin uses, written out by hand from the Linux uapi (drm.h, drm_mode.h) so the
// build needs no kernel or libdrm headers. These are stable kernel ABI.
#ifndef DRM_MIN_H
#define DRM_MIN_H
#include <stdint.h>
#include <sys/ioctl.h>

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_fb_cmd {          // GETFB: the legacy single-plane description
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_fb_cmd2 {         // GETFB2: pixel format, per-plane handles, pitches, offsets, modifiers
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};
#define DRM_MODE_FB_MODIFIERS (1 << 1)

struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_gem_close { uint32_t handle, pad; };

#define DRM_IOCTL_BASE 'd'
#define DRM_IOCTL_GEM_CLOSE         _IOW(DRM_IOCTL_BASE, 0x09, struct drm_gem_close)
#define DRM_IOCTL_DROP_MASTER       _IO(DRM_IOCTL_BASE, 0x1f)
#define DRM_IOCTL_MODE_GETRESOURCES _IOWR(DRM_IOCTL_BASE, 0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      _IOWR(DRM_IOCTL_BASE, 0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETFB        _IOWR(DRM_IOCTL_BASE, 0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_MAP_DUMB     _IOWR(DRM_IOCTL_BASE, 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_GETFB2       _IOWR(DRM_IOCTL_BASE, 0xCE, struct drm_mode_fb_cmd2)

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_XRGB8888 FOURCC('X', 'R', '2', '4')
#define FMT_ARGB8888 FOURCC('A', 'R', '2', '4')
#define FMT_XBGR8888 FOURCC('X', 'B', '2', '4')
#define FMT_ABGR8888 FOURCC('A', 'B', '2', '4')
#define FMT_RGB565   FOURCC('R', 'G', '1', '6')
#define DRM_FORMAT_MOD_LINEAR 0ULL

#endif
