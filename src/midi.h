// MIDI to and from MPC through the ALSA sequencer: a client named "MPC Remote" with a port "Out" (the addin plays
// into MPC; MPC sees it as a new MIDI input and connects it without a restart) and a port "In" (what MPC or anything
// else sends to it). libasound is loaded at run time (MPC already has it), and opened only when first used.
#ifndef MIDI_H
#define MIDI_H
#include <stddef.h>
#include <stdint.h>

// Open the client and ports if they aren't yet. 0 on success, -1 (midi_error() says why).
int midi_open(void);
const char *midi_error(void);
// The client's number, or -1.
int midi_client(void);
// Send one MIDI message: a channel message (2 or 3 bytes), a system real-time byte (F8, FA, FB, FC) or a whole
// system-exclusive message (F0 .. F7). 0 on success.
int midi_send(const uint8_t *msg, size_t n);

// Ports of other clients, for the buttons. midi_find_port: the first port whose client name contains cname (NULL: any)
// and whose name contains pname (NULL: any); user_only skips the hardware's kernel clients; label (if given) describes it.
// midi_send_to: a note on or off (3 bytes) to that port by address, through a port nothing can subscribe to.
// midi_watch: send what that port sends to In (on = 1), or stop (0).
int midi_find_port(const char *cname, const char *pname, int user_only, int *client, int *port, char *label, size_t ln);
int midi_send_to(int client, int port, const uint8_t *note3);
int midi_watch(int client, int port, int on);

struct midi_msg {
    int ms;                 // milliseconds since the listen started
    uint8_t b[3];           // the message (sysex: F0, then len holds its length and only F0 is kept)
    int len;
};
// Throw away whatever arrived on In before now.
void midi_drain(void);
// Collect what arrives on In for ms milliseconds, at most max messages. Returns the count.
int midi_listen(int ms, struct midi_msg *out, int max);

#endif
