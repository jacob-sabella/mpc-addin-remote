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
    int cc;             // 1: a knob (a relative encoder on controller `note`), 0: a button (a note)
};

// Where the user's profile (and what learn records) lives; "" for none. Call before the rest.
void buttons_init(const char *file);
// Upper-case a name, with spaces and '_' as '-'. 0 on success, -1 if it is empty, too long or has other characters.
int buttons_norm(const char *in, char *out, size_t n);
// One line of a profile file: NAME=note [channel], or NAME=ccN [channel] for a knob (# starts a comment). 0 on success, -1 if it isn't one.
int buttons_parse_line(const char *line, struct button *b);

// The profile now (rebuilt on each call: a Force appearing, buttons.conf edited). Returns the count.
int buttons_list(struct button *out, int max);
// Why there are no buttons, or where they are sent: a sentence for the user.
const char *buttons_status(void);

// Press a button (down, hold_ms, up). 0 on success, else err says why.
int buttons_press(const char *name, int hold_ms, char *err, size_t en);
// Turn a knob (a profile entry NAME=ccN): |steps| relative-encoder clicks, clockwise for steps > 0 (controller value 1),
// counter-clockwise for steps < 0 (127). 0 on success, else err says why.
int buttons_turn(const char *name, int steps, char *err, size_t en);
// Hold a button down (down = 1) or let it go (0), for a page that holds it (SHIFT while tapping the screen). A button
// held is released by itself 4 s after its last "down", so a page that goes away can't leave one stuck.
int buttons_set(const char *name, int down, char *err, size_t en);
// Wait up to timeout_ms for a press on the device's own buttons, and record it as name. 0 on success, else err says why.
int buttons_learn(const char *name, int timeout_ms, int *note, int *channel, char *err, size_t en);
// Turn the data dial by delta steps (positive = clockwise/right, negative = counter-clockwise/left). 0 on success.
int buttons_dial(int delta, char *err, size_t en);


// One channel message heard on the device's own buttons (a note, a controller, a bend ...): the Force table's name for it when it has one ("" otherwise).
struct button_event { int kind, note, channel, velocity, on; char match[24]; };   // note = first data byte, velocity = second
// Collect the channel messages the device's controls send MPC for ms (button presses and releases, knobs, wheel). Returns the count, -1 with err when
// MPC's button input isn't hooked by the addin.
int buttons_listen(int ms, struct button_event *out, int max, char *err, size_t en);

#endif
