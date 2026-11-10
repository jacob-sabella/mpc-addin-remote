#define _GNU_SOURCE
#include "surface.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// ---- the hardware's stream ----
static int data_len(uint8_t st)   // data bytes after a status byte
{
    if (st < 0xC0 || (st >= 0xE0 && st < 0xF0)) return 2;
    if (st < 0xE0) return 1;
    return st == 0xF2 ? 2 : st == 0xF1 || st == 0xF3 ? 1 : 0;
}

void surface_track_feed(struct surface_track *t, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xF8) continue;                         // real-time bytes go anywhere and change nothing
        if (c == 0xF0) { t->sysex = 1; t->need = 0; t->status = 0; continue; }
        if (c == 0xF7) { t->sysex = 0; t->need = 0; continue; }
        if (c & 0x80) {
            t->sysex = 0;
            t->need = data_len(c);
            t->status = c < 0xF0 ? c : 0;                // system common messages cancel running status
            continue;
        }
        if (t->sysex) continue;
        if (t->need) t->need--;
        else if (t->status) t->need = data_len(t->status) - 1;   // running status: this is the first data byte
    }
}

int surface_track_boundary(const struct surface_track *t) { return !t->sysex && !t->need; }

// ---- the hooks ----
typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_rawmidi_info snd_rawmidi_info_t;

static struct {
    int (*open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
    ssize_t (*read)(snd_rawmidi_t *, void *, size_t);
    int (*close)(snd_rawmidi_t *);
    int (*poll)(struct pollfd *, nfds_t, int);
} R;
static pthread_once_t once = PTHREAD_ONCE_INIT;

// The next definition of a symbol after this library, by libc's own dlsym called from here. An addin that exports its
// dlsym (the commander addin before 1.0.1) answers RTLD_NEXT relative to itself, which skips every shim between this
// library and it; dlvsym is a different symbol, so libc's dlsym is reachable and takes this library as the caller.
static void *next_sym(const char *name)
{
    static void *(*libc_dlsym)(void *, const char *);
    if (!libc_dlsym) {
        // the versions of libc's dlsym: 2.34 moved it from libdl into libc
        static const char *const vers[] = {"2.34", "2.4", "2.17", "2.2.5", "2.0"};
        for (size_t i = 0; i < sizeof vers / sizeof *vers && !libc_dlsym; i++) {
            char v[16] = "GLIBC_";
            strcat(v, vers[i]);
            libc_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_DEFAULT, "dlsym", v);
        }
    }
    return (libc_dlsym ? libc_dlsym : dlsym)(RTLD_NEXT, name);
}

static void load(void)
{
    *(void **)&R.open = next_sym("snd_rawmidi_open");   // whatever comes next: another shim, or libasound
    *(void **)&R.read = next_sym("snd_rawmidi_read");
    *(void **)&R.close = next_sym("snd_rawmidi_close");
    *(void **)&R.poll = next_sym("poll");
}

#define QMAX 64
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static int enabled;
static _Atomic(snd_rawmidi_t *) priv_in;
static atomic_int priv_fd = -1;
static int wake[2] = { -1, -1 };
static uint8_t q[QMAX][3];
static int qhead, qcount, qoff;                // qoff: bytes of the head message already read (MPC read part of it)
static struct surface_track hw;
static uint8_t restatus;                       // put back the hardware's running status before its next data byte
static char status[200];

#define TAPN 128
static uint8_t tap[TAPN][3];
static unsigned long tap_total;                // messages seen since the addin loaded
static struct { uint8_t status, d[2]; int have, need; int sysex; } tp;   // the tap's own parser of the hardware stream

static void tap_feed_locked(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xF8) continue;
        if (c == 0xF0) { tp.sysex = 1; tp.status = 0; tp.have = 0; continue; }
        if (c == 0xF7) { tp.sysex = 0; continue; }
        if (c & 0x80) { tp.sysex = 0; tp.have = 0; tp.status = c < 0xF0 ? c : 0; tp.need = tp.status ? data_len(c) : 0; continue; }
        if (tp.sysex || !tp.status) continue;
        tp.d[tp.have++] = c;
        if (tp.have == tp.need) {
            if (tp.need == 2) {
                uint8_t *e = tap[tap_total++ % TAPN];
                e[0] = tp.status; e[1] = tp.d[0]; e[2] = tp.d[1];
            }
            tp.have = 0;   // running status: the next data byte starts another message
        }
    }
}

unsigned long surface_tap_pos(void)
{
    pthread_mutex_lock(&mtx);
    unsigned long p = tap_total;
    pthread_mutex_unlock(&mtx);
    return p;
}

int surface_tap_next(unsigned long *pos, uint8_t m[3])
{
    int r = 0;
    pthread_mutex_lock(&mtx);
    if (tap_total - *pos > TAPN) { *pos = tap_total - TAPN; r = -1; }
    else if (*pos < tap_total) { memcpy(m, tap[*pos % TAPN], 3); (*pos)++; r = 1; }
    pthread_mutex_unlock(&mtx);
    return r;
}

void surface_enable(int on) { enabled = on; }
int surface_active(void) { return atomic_load(&priv_in) != NULL; }
const char *surface_status(void) { return status; }

static int ready_locked(void) { return qcount && (qoff || surface_track_boundary(&hw)); }

int surface_send(const uint8_t m[3])
{
    pthread_mutex_lock(&mtx);
    int r = -1;
    if (atomic_load(&priv_in) && qcount < QMAX) {
        memcpy(q[(qhead + qcount) % QMAX], m, 3);
        qcount++;
        r = 0;
    }
    pthread_mutex_unlock(&mtx);
    if (!r && wake[1] >= 0) { char c = 1; if (write(wake[1], &c, 1) < 0) { /* full: a wake is pending anyway */ } }
    return r;
}

// The same test as the button remap and skipback shims: the port's name or subdevice name has "Private" in it.
static int is_private(snd_rawmidi_t *h, char *names, size_t nn)
{
    size_t (*info_sizeof)(void) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_sizeof");
    int (*info)(snd_rawmidi_t *, snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info");
    const char *(*subname)(const snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_subdevice_name");
    const char *(*devname)(const snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_name");
    int found = 0;
    if (!info_sizeof || !info || !subname || !devname) return 0;
    snd_rawmidi_info_t *ri = calloc(1, info_sizeof());
    if (ri && info(h, ri) == 0) {
        const char *sn = subname(ri), *dn = devname(ri);
        found = (sn && strstr(sn, "Private")) || (dn && strstr(dn, "Private"));
        snprintf(names, nn, "%s", sn && *sn ? sn : dn ? dn : "?");
    }
    free(ri);
    return found;
}

__attribute__((visibility("default"))) int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode)
{
    pthread_once(&once, load);
    if (!R.open) return -ENOSYS;
    int r = R.open(in, out, name, mode);
    char names[96];
    if (r < 0 || !enabled || !in || !*in || !is_private(*in, names, sizeof names)) return r;
    int (*pdesc)(snd_rawmidi_t *, struct pollfd *, unsigned int) = dlsym(RTLD_DEFAULT, "snd_rawmidi_poll_descriptors");
    struct pollfd p;
    int fd = pdesc && pdesc(*in, &p, 1) == 1 ? p.fd : -1;
    pthread_mutex_lock(&mtx);
    if (wake[0] < 0 && pipe2(wake, O_NONBLOCK | O_CLOEXEC)) wake[0] = wake[1] = -1;
    qhead = qcount = qoff = 0;
    memset(&hw, 0, sizeof hw);
    restatus = 0;
    atomic_store(&priv_fd, fd);
    atomic_store(&priv_in, *in);
    snprintf(status, sizeof status, "Presses go straight into MPC's input from %s%s.", names,
             fd < 0 || wake[0] < 0 ? " (MPC may only see them with the next hardware event)" : "");
    pthread_mutex_unlock(&mtx);
    return r;
}

__attribute__((visibility("default"))) int snd_rawmidi_close(snd_rawmidi_t *h)
{
    pthread_once(&once, load);
    if (!R.close) return -ENOSYS;
    if (h && h == atomic_load(&priv_in)) {
        pthread_mutex_lock(&mtx);
        atomic_store(&priv_in, NULL);
        atomic_store(&priv_fd, -1);
        qcount = qoff = 0;
        snprintf(status, sizeof status, "MPC closed its button input; it is hooked again when MPC reopens it.");
        pthread_mutex_unlock(&mtx);
    }
    return R.close(h);
}

__attribute__((visibility("default"))) ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buffer, size_t size)
{
    pthread_once(&once, load);
    if (!R.read) return -ENOSYS;
    if (!size || h != atomic_load_explicit(&priv_in, memory_order_relaxed)) return R.read(h, buffer, size);
    uint8_t *o = buffer;
    pthread_mutex_lock(&mtx);
    if (ready_locked()) {   // the queued presses first: the hardware is between two messages
        size_t n = 0;
        while (n < size && qcount) {
            o[n++] = q[qhead][qoff++];
            if (qoff == 3) { qoff = 0; qhead = (qhead + 1) % QMAX; qcount--; }
        }
        restatus = hw.status;
        pthread_mutex_unlock(&mtx);
        return (ssize_t)n;
    }
    int prefix = restatus && size > 1;
    pthread_mutex_unlock(&mtx);
    ssize_t r = R.read(h, o + prefix, size - (size_t)prefix);   // MPC's reader is the only one: no lock while it waits
    if (r <= 0) return r;
    pthread_mutex_lock(&mtx);
    if (prefix) {
        if (restatus && o[1] < 0x80) { o[0] = restatus; r++; }
        else memmove(o, o + 1, (size_t)r);
    }
    restatus = 0;
    surface_track_feed(&hw, o, (size_t)r);
    tap_feed_locked(o, (size_t)r);
    pthread_mutex_unlock(&mtx);
    return r;
}

// MPC waits for its button input in poll(): it also wakes for queued presses.
__attribute__((visibility("default"))) int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    pthread_once(&once, load);
    if (!R.poll) { errno = ENOSYS; return -1; }
    int pfd = atomic_load_explicit(&priv_fd, memory_order_relaxed), idx = -1;
    if (pfd >= 0 && wake[0] >= 0 && nfds < 15)
        for (nfds_t i = 0; i < nfds; i++)
            if (fds[i].fd == pfd && (fds[i].events & POLLIN)) idx = (int)i;
    if (idx < 0) return R.poll(fds, nfds, timeout);

    pthread_mutex_lock(&mtx);
    int ready = ready_locked();
    pthread_mutex_unlock(&mtx);
    if (ready) {
        for (nfds_t i = 0; i < nfds; i++) fds[i].revents = 0;
        fds[idx].revents = POLLIN;
        return 1;
    }
    struct pollfd all[16];
    memcpy(all, fds, nfds * sizeof *fds);
    all[nfds] = (struct pollfd){ .fd = wake[0], .events = POLLIN };
    int r = R.poll(all, nfds + 1, timeout);
    if (r < 0) return r;
    for (nfds_t i = 0; i < nfds; i++) fds[i].revents = all[i].revents;
    if (all[nfds].revents) {
        char drain[64];
        while (read(wake[0], drain, sizeof drain) > 0) {}
        r--;
        pthread_mutex_lock(&mtx);
        ready = ready_locked();
        pthread_mutex_unlock(&mtx);
        if (ready && !fds[idx].revents) { fds[idx].revents = POLLIN; r++; }
    }
    return r;
}

__attribute__((visibility("default"))) int __poll_chk(struct pollfd *fds, nfds_t nfds, int timeout, size_t len)
{
    (void)len;
    return poll(fds, nfds, timeout);
}
