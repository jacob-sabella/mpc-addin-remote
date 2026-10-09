// The device's hardware buttons (PLAY, MIXER, the arrows ...), pressed from here. MPC's control surface sends each
// button as a MIDI note on/off to MPC; the addin sends the same note to MPC's own port for it (a "Virtual RawMIDI"
// port), so MPC sees a press. Which note is which button is a profile: a built-in table for the Akai Force (found by
// its MIDI client), plus buttons.conf next to the addin (NAME=note [channel]), which also holds what "learn" records
// (press a button on the device and the addin writes its note under the name given). Other devices start with an
// empty profile and learn theirs.
#ifndef BUTTONS_H
#define BUTTONS_H
#include <stddef.h>

struct button {
    char name[24];      // upper case, letters, digits and '-'
    int note;           // 0..127
    int channel;        // 1..16
    int learned;        // 1: from buttons.conf, 0: built in
};

// Where the user's profile (and what learn records) lives; "" for none. Call before the rest.
void buttons_init(const char *file);
// Upper-case a name, with spaces and '_' as '-'. 0 on success, -1 if it is empty, too long or has other characters.
int buttons_norm(const char *in, char *out, size_t n);
// One line of a profile file: NAME=note [channel] (# starts a comment). 0 on success, -1 if it isn't one.
int buttons_parse_line(const char *line, struct button *b);

// The profile now (rebuilt on each call: a Force appearing, buttons.conf edited). Returns the count.
int buttons_list(struct button *out, int max);
// Why there are no buttons, or where they are sent: a sentence for the user.
const char *buttons_status(void);

// Press a button (down, hold_ms, up). 0 on success, else err says why.
int buttons_press(const char *name, int hold_ms, char *err, size_t en);
// Wait up to timeout_ms for a press on the device's own buttons, and record it as name. 0 on success, else err says why.
int buttons_learn(const char *name, int timeout_ms, int *note, int *channel, char *err, size_t en);

#endif
