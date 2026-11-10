// src/surface.c against tests/fake_asound.c: presses queued for MPC come out of its reads between the hardware's
// messages (never inside a sysex or a message), the hardware's running status survives them, poll() wakes for them,
// and other ports and a disabled addin are left alone.
#include "../src/surface.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

typedef struct _snd_rawmidi snd_rawmidi_t;
int snd_rawmidi_open(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
ssize_t snd_rawmidi_read(snd_rawmidi_t *, void *, size_t);
int snd_rawmidi_close(snd_rawmidi_t *);
int snd_rawmidi_poll_descriptors(snd_rawmidi_t *, struct pollfd *, unsigned int);
void fake_hw_write(const void *, size_t);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// What MPC would do: wait in poll(), then read. Returns the bytes read (0: poll timed out).
static int mpc_read(snd_rawmidi_t *h, unsigned char *b, size_t n, int timeout)
{
    struct pollfd p[2] = { { .fd = 0, .events = 0 } };
    snd_rawmidi_poll_descriptors(h, &p[1], 1);
    if (poll(p, 2, timeout) <= 0 || !(p[1].revents & POLLIN)) return 0;
    return (int)snd_rawmidi_read(h, b, n);
}

int main(void)
{
    struct surface_track t = { 0 };
    const unsigned char sx[] = { 0xF0, 1, 2 }, sx_end[] = { 3, 0xF7 };
    surface_track_feed(&t, sx, 3); CHECK(!surface_track_boundary(&t));
    surface_track_feed(&t, sx_end, 2); CHECK(surface_track_boundary(&t));
    const unsigned char half[] = { 0x99, 36 }, rest[] = { 100 }, run[] = { 37, 90 }, prog[] = { 0xC0 };
    surface_track_feed(&t, half, 2); CHECK(!surface_track_boundary(&t) && t.status == 0x99);
    surface_track_feed(&t, rest, 1); CHECK(surface_track_boundary(&t));
    surface_track_feed(&t, run, 1); CHECK(!surface_track_boundary(&t));
    surface_track_feed(&t, run + 1, 1); CHECK(surface_track_boundary(&t) && t.status == 0x99);
    const unsigned char clk[] = { 0x99, 0xF8, 38 };
    surface_track_feed(&t, clk, 3); CHECK(!surface_track_boundary(&t));   // a clock byte inside a message changes nothing
    surface_track_feed(&t, prog, 1); CHECK(!surface_track_boundary(&t) && t.need == 1);

    const unsigned char play[3] = { 0x90, 82, 127 };
    unsigned char b[64];
    snd_rawmidi_t *in, *pub;

    // off: nothing is hooked
    CHECK(snd_rawmidi_open(&in, NULL, "hw:1,0,1", 0) == 0 && !surface_active() && surface_send(play) == -1);
    snd_rawmidi_close(in);

    surface_enable(1);
    CHECK(snd_rawmidi_open(&pub, NULL, "hw:1,0,0", 0) == 0 && !surface_active());   // the Public port isn't the buttons
    CHECK(snd_rawmidi_open(&in, NULL, "hw:1,0,1", 0) == 0 && surface_active() && strstr(surface_status(), "Private"));

    // a press while MPC waits in poll(): it wakes and reads the press
    CHECK(surface_send(play) == 0);
    CHECK(mpc_read(in, b, sizeof b, 1000) == 3 && !memcmp(b, play, 3));
    CHECK(mpc_read(in, b, sizeof b, 50) == 0);                                    // nothing else is there

    // the hardware is half way through a sysex: the press waits for its end
    const unsigned char hb1[] = { 0xF0, 0x47, 0x7F }, hb2[] = { 0x40, 0x01, 0xF7 };
    fake_hw_write(hb1, 3);
    CHECK(mpc_read(in, b, sizeof b, 1000) == 3 && !memcmp(b, hb1, 3));
    CHECK(surface_send(play) == 0);
    fake_hw_write(hb2, 3);
    CHECK(mpc_read(in, b, sizeof b, 1000) == 3 && !memcmp(b, hb2, 3));
    CHECK(mpc_read(in, b, sizeof b, 1000) == 3 && !memcmp(b, play, 3));

    // a pad (ch 10) then a press, then the pad's release in running status: its status byte is put back
    const unsigned char pad[] = { 0x99, 36, 100 }, pad_off_run[] = { 36, 0 };
    unsigned long tp = surface_tap_pos();
    fake_hw_write(pad, 3);
    CHECK(mpc_read(in, b, sizeof b, 1000) == 3);
    CHECK(surface_send(play) == 0);
    fake_hw_write(pad_off_run, 2);
    CHECK(mpc_read(in, b, sizeof b, 1000) == 3 && !memcmp(b, play, 3));
    CHECK(mpc_read(in, b, sizeof b, 1000) == 3 && b[0] == 0x99 && b[1] == 36 && b[2] == 0);
    {   // the tap saw the hardware's two messages (the release's status restored from running status), not the press
        uint8_t m[3];
        CHECK(surface_tap_next(&tp, m) == 1 && m[0] == 0x99 && m[1] == 36 && m[2] == 100);
        CHECK(surface_tap_next(&tp, m) == 1 && m[0] == 0x99 && m[1] == 36 && m[2] == 0);
        CHECK(surface_tap_next(&tp, m) == 0);
    }

    // MPC reading one byte at a time gets the whole press, in order
    CHECK(surface_send(play) == 0);
    for (int i = 0; i < 3; i++) CHECK(mpc_read(in, b, 1, 1000) == 1 && b[0] == play[i]);

    // a full queue says so
    int sent = 0;
    while (surface_send(play) == 0 && sent < 1000) sent++;
    CHECK(sent == 64);
    int got = 0, r;
    while ((r = mpc_read(in, b, sizeof b, 50)) > 0) got += r;
    CHECK(got == 64 * 3);

    // closed: not hooked any more
    snd_rawmidi_close(in);
    CHECK(!surface_active() && surface_send(play) == -1);
    snd_rawmidi_close(pub);

    printf(fails ? "surface: %d FAILED\n" : "surface: all passed\n", fails);
    return fails != 0;
}
