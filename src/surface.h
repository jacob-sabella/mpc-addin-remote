// MPC's button input, pressable without MockbaMod. MPC reads the device's buttons as raw MIDI from its control
// surface's "Private" port and waits for them in poll(). The addin is preloaded ahead of libasound, so it sees MPC open
// that port and adds the presses it is asked for (src/buttons.c) to what MPC reads from it, between the hardware's own
// messages, and wakes MPC's poll() for them. Other shims on the same port (the button remap, skipback) still see the
// hardware's bytes as before. When MPC gets something else for that port (MockbaMod's MidiLoop hands it a virtual
// one), nothing is hooked and the buttons go through that port's sequencer side instead.
#ifndef SURFACE_H
#define SURFACE_H
#include <stddef.h>
#include <stdint.h>

// On (1) or off (0); call before MPC opens its MIDI (the constructor does). Off by default: tests and other hosts.
void surface_enable(int on);
// 1 while MPC has the control surface's input open through the addin.
int surface_active(void);
// Queue a 3-byte channel message for MPC to read as if from the control surface. 0 on success, -1 when it isn't
// hooked or the queue is full.
int surface_send(const uint8_t m[3]);
// A read-only tap on what the hardware sends MPC (3-byte channel messages only, in order). surface_tap_pos() is where
// the log is now; surface_tap_next(&pos, m) hands over the message at pos and moves on, 0 when there is none yet
// (-1 and pos jumps to the oldest one kept when the reader fell more than the log's size behind).
unsigned long surface_tap_pos(void);
int surface_tap_next(unsigned long *pos, uint8_t m[3]);
// What happened, a sentence for the user ("" when MPC hasn't opened its button input since the addin loaded).
const char *surface_status(void);

// Where the hardware's byte stream is, so a message is only added between two of its own (exposed for the tests).
struct surface_track {
    int need;           // data bytes still to come for the current message
    int sysex;          // inside F0 .. F7
    uint8_t status;     // the running status (a channel message's status byte), 0 for none
};
void surface_track_feed(struct surface_track *t, const uint8_t *b, size_t n);
int surface_track_boundary(const struct surface_track *t);

#endif
