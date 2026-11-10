// A stand-in for libasound's raw MIDI calls, for tests/surface_test.c: one "Private" port whose bytes the test writes
// into a pipe (fake_hw_write), as the control surface would.
#define _GNU_SOURCE
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct _snd_rawmidi { int fd; char sub[64]; } snd_rawmidi_t;
typedef struct _snd_rawmidi_info { char sub[64]; } snd_rawmidi_info_t;
static int hw_w = -1;
int fake_opens, fake_closes;

int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode)
{
    (void)mode;
    int p[2];
    if (out) *out = NULL;
    if (!in || pipe(p)) return -1;
    snd_rawmidi_t *h = calloc(1, sizeof *h);
    h->fd = p[0];
    hw_w = p[1];
    strcpy(h->sub, strstr(name, ",1") ? "Akai Pro Force Private" : "Akai Pro Force Public");
    *in = h;
    fake_opens++;
    return 0;
}
ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *b, size_t n) { return read(h->fd, b, n); }
int snd_rawmidi_close(snd_rawmidi_t *h) { close(h->fd); free(h); fake_closes++; return 0; }
size_t snd_rawmidi_info_sizeof(void) { return sizeof(snd_rawmidi_info_t); }
int snd_rawmidi_info(snd_rawmidi_t *h, snd_rawmidi_info_t *i) { strcpy(i->sub, h->sub); return 0; }
const char *snd_rawmidi_info_get_subdevice_name(const snd_rawmidi_info_t *i) { return i->sub; }
const char *snd_rawmidi_info_get_name(const snd_rawmidi_info_t *i) { (void)i; return "Akai Pro Force"; }
int snd_rawmidi_poll_descriptors(snd_rawmidi_t *h, struct pollfd *p, unsigned int n)
{
    if (n < 1) return 0;
    p->fd = h->fd; p->events = POLLIN;
    return 1;
}
void fake_hw_write(const void *b, size_t n) { if (write(hw_w, b, n) != (ssize_t)n) abort(); }
