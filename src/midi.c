#define _GNU_SOURCE
#include "midi.h"
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// The parts of alsa/asoundlib.h used here. snd_seq_event_t is 28 bytes and hasn't changed since ALSA 1.0;
// tests/unit_test.c checks this layout against the real header where it is installed.
typedef struct _snd_seq snd_seq_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    struct { unsigned int sec, nsec; } time;
    struct { unsigned char client, port; } source, dest;
    union {
        struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } note;
        struct { unsigned char channel, unused[3]; unsigned int param; int value; } control;
        struct __attribute__((packed)) { unsigned int len; void *ptr; } ext;
        unsigned char raw8[12];
    } data;
} seq_event_t;
_Static_assert(sizeof(seq_event_t) == 28, "snd_seq_event_t layout");

enum {
    OPEN_DUPLEX = 3, NONBLOCK = 1,
    EV_NOTEON = 6, EV_NOTEOFF = 7, EV_KEYPRESS = 8, EV_CONTROLLER = 10, EV_PGMCHANGE = 11, EV_CHANPRESS = 12,
    EV_PITCHBEND = 13, EV_START = 30, EV_CONTINUE = 31, EV_STOP = 32, EV_CLOCK = 36, EV_SYSEX = 130,
    LENGTH_VARIABLE = 1 << 2, QUEUE_DIRECT = 253, ADDRESS_SUBSCRIBERS = 254, ADDRESS_UNKNOWN = 253,
    CAP_READ = 1 << 0, CAP_WRITE = 1 << 1, CAP_SUBS_READ = 1 << 5, CAP_SUBS_WRITE = 1 << 6,
    TYPE_MIDI_GENERIC = 1 << 1, TYPE_APPLICATION = 1 << 20,
};

static struct {
    int (*open)(snd_seq_t **, const char *, int, int);
    int (*set_client_name)(snd_seq_t *, const char *);
    int (*create_simple_port)(snd_seq_t *, const char *, unsigned int, unsigned int);
    int (*client_id)(snd_seq_t *);
    int (*event_output_direct)(snd_seq_t *, seq_event_t *);
    int (*event_input)(snd_seq_t *, seq_event_t **);
    int (*poll_count)(snd_seq_t *, short);
    int (*poll_descriptors)(snd_seq_t *, struct pollfd *, unsigned int, short);
} A;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static snd_seq_t *seq;
static int out_port = -1, in_port = -1, client = -1, tried;
static char err[96] = "not opened yet";
static struct pollfd pfd[4];
static int npfd;

static int load(void)
{
    void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return -1;
    *(void **)&A.open = dlsym(lib, "snd_seq_open");
    *(void **)&A.set_client_name = dlsym(lib, "snd_seq_set_client_name");
    *(void **)&A.create_simple_port = dlsym(lib, "snd_seq_create_simple_port");
    *(void **)&A.client_id = dlsym(lib, "snd_seq_client_id");
    *(void **)&A.event_output_direct = dlsym(lib, "snd_seq_event_output_direct");
    *(void **)&A.event_input = dlsym(lib, "snd_seq_event_input");
    *(void **)&A.poll_count = dlsym(lib, "snd_seq_poll_descriptors_count");
    *(void **)&A.poll_descriptors = dlsym(lib, "snd_seq_poll_descriptors");
    if (A.open && A.set_client_name && A.create_simple_port && A.client_id && A.event_output_direct && A.event_input
        && A.poll_count && A.poll_descriptors) return 0;
    dlclose(lib);   // the library stays loaded if MPC has it; only this reference goes
    return -1;
}

int midi_open(void)
{
    pthread_mutex_lock(&mtx);
    if (!seq && !tried) {            // one attempt: a failure doesn't change while MPC runs
        tried = 1;
        if (load()) snprintf(err, sizeof err, "libasound.so.2 isn't available");
        else if (A.open(&seq, "default", OPEN_DUPLEX, NONBLOCK) < 0) {
            seq = NULL;
            snprintf(err, sizeof err, "the ALSA sequencer can't be opened (/dev/snd/seq)");
        } else {
            A.set_client_name(seq, "MPC Remote");
            out_port = A.create_simple_port(seq, "Out", CAP_READ | CAP_SUBS_READ, TYPE_MIDI_GENERIC | TYPE_APPLICATION);
            in_port = A.create_simple_port(seq, "In", CAP_WRITE | CAP_SUBS_WRITE, TYPE_MIDI_GENERIC | TYPE_APPLICATION);
            client = A.client_id(seq);
            int n = A.poll_count(seq, POLLIN);
            npfd = A.poll_descriptors(seq, pfd, (unsigned)(n > 4 ? 4 : n < 0 ? 0 : n), POLLIN);
            if (npfd < 0) npfd = 0;
            if (out_port < 0) snprintf(err, sizeof err, "the sequencer port can't be created");
            else err[0] = 0;
        }
    }
    int r = seq && out_port >= 0 ? 0 : -1;
    pthread_mutex_unlock(&mtx);
    return r;
}

const char *midi_error(void) { return err; }

int midi_client(void)
{
    pthread_mutex_lock(&mtx);
    int c = seq ? client : -1;
    pthread_mutex_unlock(&mtx);
    return c;
}

int midi_send(const uint8_t *m, size_t n)
{
    if (!n || midi_open()) return -1;
    seq_event_t ev;
    memset(&ev, 0, sizeof ev);
    uint8_t st = m[0], ch = st & 15;
    switch (st & 0xF0) {
    case 0x80: case 0x90:
        if (n < 3) return -1;
        ev.type = (st & 0xF0) == 0x90 ? EV_NOTEON : EV_NOTEOFF;
        ev.data.note.channel = ch; ev.data.note.note = m[1] & 127; ev.data.note.velocity = m[2] & 127;
        break;
    case 0xA0:
        if (n < 3) return -1;
        ev.type = EV_KEYPRESS;
        ev.data.note.channel = ch; ev.data.note.note = m[1] & 127; ev.data.note.velocity = m[2] & 127;
        break;
    case 0xB0:
        if (n < 3) return -1;
        ev.type = EV_CONTROLLER;
        ev.data.control.channel = ch; ev.data.control.param = m[1] & 127u; ev.data.control.value = m[2] & 127;
        break;
    case 0xC0: case 0xD0:
        if (n < 2) return -1;
        ev.type = (st & 0xF0) == 0xC0 ? EV_PGMCHANGE : EV_CHANPRESS;
        ev.data.control.channel = ch; ev.data.control.value = m[1] & 127;
        break;
    case 0xE0:
        if (n < 3) return -1;
        ev.type = EV_PITCHBEND;
        ev.data.control.channel = ch; ev.data.control.value = ((m[2] & 127) << 7 | (m[1] & 127)) - 8192;
        break;
    default:
        if (st == 0xF0) {
            if (n < 2 || m[n - 1] != 0xF7) return -1;
            ev.type = EV_SYSEX;
            ev.flags = LENGTH_VARIABLE;
            ev.data.ext.len = (unsigned)n;
            ev.data.ext.ptr = (void *)m;
        } else if (st == 0xF8) ev.type = EV_CLOCK;
        else if (st == 0xFA) ev.type = EV_START;
        else if (st == 0xFB) ev.type = EV_CONTINUE;
        else if (st == 0xFC) ev.type = EV_STOP;
        else return -1;
    }
    ev.queue = QUEUE_DIRECT;
    ev.dest.client = ADDRESS_SUBSCRIBERS;
    ev.dest.port = ADDRESS_UNKNOWN;
    pthread_mutex_lock(&mtx);
    ev.source.port = (unsigned char)out_port;
    int r = A.event_output_direct(seq, &ev);
    pthread_mutex_unlock(&mtx);
    return r < 0 ? -1 : 0;
}

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Everything waiting on In, into out (if given). Called with mtx held.
static int take(struct midi_msg *out, int max, int ms)
{
    int k = 0;
    seq_event_t *ev;
    while (A.event_input(seq, &ev) >= 0 && ev) {
        if (!out || k >= max || ev->dest.port != in_port) continue;
        struct midi_msg m = { .ms = ms, .len = 3 };
        int ch = ev->data.note.channel & 15;
        switch (ev->type) {
        case EV_NOTEON: case EV_NOTEOFF: case EV_KEYPRESS:
            m.b[0] = (uint8_t)((ev->type == EV_NOTEON ? 0x90 : ev->type == EV_NOTEOFF ? 0x80 : 0xA0) | ch);
            m.b[1] = ev->data.note.note & 127; m.b[2] = ev->data.note.velocity & 127;
            break;
        case EV_CONTROLLER:
            m.b[0] = (uint8_t)(0xB0 | (ev->data.control.channel & 15));
            m.b[1] = ev->data.control.param & 127; m.b[2] = (uint8_t)(ev->data.control.value & 127);
            break;
        case EV_PGMCHANGE: case EV_CHANPRESS:
            m.b[0] = (uint8_t)((ev->type == EV_PGMCHANGE ? 0xC0 : 0xD0) | (ev->data.control.channel & 15));
            m.b[1] = (uint8_t)(ev->data.control.value & 127); m.len = 2;
            break;
        case EV_PITCHBEND: {
            int v = ev->data.control.value + 8192;
            v = v < 0 ? 0 : v > 16383 ? 16383 : v;
            m.b[0] = (uint8_t)(0xE0 | (ev->data.control.channel & 15)); m.b[1] = v & 127; m.b[2] = (uint8_t)(v >> 7);
            break;
        }
        case EV_START: m.b[0] = 0xFA; m.len = 1; break;
        case EV_CONTINUE: m.b[0] = 0xFB; m.len = 1; break;
        case EV_STOP: m.b[0] = 0xFC; m.len = 1; break;
        case EV_CLOCK: m.b[0] = 0xF8; m.len = 1; break;
        case EV_SYSEX: m.b[0] = 0xF0; m.len = (int)ev->data.ext.len; break;
        default: continue;
        }
        out[k++] = m;
    }
    return k;
}

void midi_drain(void)
{
    if (midi_open()) return;
    pthread_mutex_lock(&mtx);
    take(NULL, 0, 0);
    pthread_mutex_unlock(&mtx);
}

int midi_listen(int ms, struct midi_msg *out, int max)
{
    if (midi_open()) return 0;
    long long t0 = now_ms(), end = t0 + ms;
    int k = 0;
    for (;;) {
        pthread_mutex_lock(&mtx);
        k += take(out + k, max - k, (int)(now_ms() - t0));
        pthread_mutex_unlock(&mtx);
        long long left = end - now_ms();
        if (left <= 0) break;
        struct pollfd p[4];
        memcpy(p, pfd, sizeof p);
        if (npfd > 0) poll(p, (nfds_t)npfd, (int)(left < 20 ? left : 20));   // the lock isn't held while waiting
        else nanosleep(&(struct timespec){ 0, 10000000 }, NULL);
    }
    return k;
}
