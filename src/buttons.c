#define _GNU_SOURCE
#include "buttons.h"
#include "midi.h"
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXB 160
static const char *const INJECT_PATHS[] = { "/dev/midi_inject", "/data/midi.in" };
static int inject_fd = -1;
static const char *inject_path;

static int try_inject(void)
{
    if (inject_fd >= 0) return 0;
    for (size_t i = 0; i < sizeof INJECT_PATHS / sizeof *INJECT_PATHS; i++) {
        inject_fd = open(INJECT_PATHS[i], O_WRONLY | O_APPEND | O_CLOEXEC);
        if (inject_fd >= 0) { inject_path = INJECT_PATHS[i]; return 0; }
    }
    return -1;
}

static int send_btn(int c, int p, const uint8_t *m)
{
    if (inject_fd >= 0 && c < 0)
        return write(inject_fd, m, 3) == 3 ? 0 : -1;
    return midi_send_to(c, p, m);
}

// The Akai Force: its buttons are MIDI notes on channel 1 (measured on a Force: press = velocity 127,
// release = velocity 0, on the MIDI port "Akai Pro Force Private").
static const struct { const char *name; int note; } FORCE[] = {
    { "KNOBS", 1 }, { "MENU", 2 }, { "MATRIX", 3 }, { "NOTE", 4 }, { "CLIP", 9 }, { "MIXER", 11 }, { "SAVE", 36 },
    { "EDIT", 37 }, { "SHIFT", 49 }, { "UNDO", 67 }, { "REC", 73 }, { "STOP", 81 }, { "PLAY", 82 }, { "MUTE", 91 },
    { "SOLO", 92 }, { "ARM", 93 }, { "UP", 112 }, { "DOWN", 113 }, { "LEFT", 114 }, { "RIGHT", 115 },
    { "LAUNCH", 116 }, { "COPY", 122 },
};

// The MPC Sample: it has no "...Private" ALSA port at all (verified 2026-10-16), so these notes only reach
// MPC's button handling through the /dev/midi_inject path (a custom kernel module; see mpc-vst-plugins
// docs), not through midi_send_to's ALSA sequencer route. Measured on a real Sample, channel 1.
static const struct { const char *name; int note; } SAMPLE[] = {
    { "MAIN", 7 }, { "BROWSE", 36 }, { "ENTER", 111 }, { "SHIFT", 49 }, { "KNOB-FX", 0 }, { "SAMPLE-EDIT", 6 },
    { "SAMPLE-REC", 74 }, { "PLUS", 54 }, { "MINUS", 55 }, { "TAP-TEMPO", 12 }, { "OVERDUB", 80 }, { "STOP", 81 },
    { "PLAY", 82 }, { "PLAY-START", 83 }, { "REC", 73 }, { "PAD-1", 84 }, { "PAD-2", 85 }, { "PAD-3", 86 },
    { "PAD-4", 87 },
};

// The MPC Live III (measured on one, MPC OS 3.9.1): channel 1 notes for its buttons, channel 10 for the pads (the
// standard bank A drum map), and relative controllers for the knobs (value 1 clockwise, 127 counter-clockwise). It has
// no CLIP, SOLO or arrows; MATRIX is SHIFT + ARRANGE, LAUNCH is SET + SEQ7 and ARM is SET + SEQ10.
static const struct { const char *name; int note, channel, cc; } LIVE3[] = {
    { "KNOBS", 0, 1, 0 }, { "SET", 10, 1, 0 }, { "EDIT", 12, 1, 0 }, { "MIXER", 33, 1, 0 }, { "MUTE", 44, 1, 0 },
    { "SHIFT", 49, 1, 0 }, { "SAVE", 50, 1, 0 }, { "MAIN", 52, 1, 0 }, { "UNDO", 67, 1, 0 }, { "REC", 73, 1, 0 },
    { "OVERDUB", 80, 1, 0 }, { "STOP", 81, 1, 0 }, { "PLAY", 82, 1, 0 }, { "PLAY-START", 83, 1, 0 },
    { "QLINK1", 84, 1, 0 }, { "QLINK2", 85, 1, 0 }, { "QLINK3", 86, 1, 0 }, { "QLINK4", 87, 1, 0 },
    { "SEQ1", 92, 1, 0 }, { "SEQ2", 93, 1, 0 }, { "SEQ3", 94, 1, 0 }, { "SEQ4", 95, 1, 0 }, { "SEQ5", 96, 1, 0 }, { "SEQ6", 97, 1, 0 }, { "SEQ7", 98, 1, 0 }, { "SEQ8", 99, 1, 0 }, { "SEQ9", 100, 1, 0 }, { "SEQ10", 101, 1, 0 }, { "SEQ11", 102, 1, 0 }, { "SEQ12", 103, 1, 0 }, { "SEQ13", 104, 1, 0 }, { "SEQ14", 105, 1, 0 }, { "SEQ15", 106, 1, 0 }, { "SEQ16", 107, 1, 0 },
    { "ARRANGE", 112, 1, 0 }, { "NOTE", 114, 1, 0 }, { "COPY", 122, 1, 0 }, { "MENU", 123, 1, 0 },
    { "PAD1", 37, 10, 0 }, { "PAD2", 36, 10, 0 }, { "PAD3", 42, 10, 0 }, { "PAD4", 82, 10, 0 }, { "PAD5", 40, 10, 0 }, { "PAD6", 38, 10, 0 }, { "PAD7", 46, 10, 0 }, { "PAD8", 44, 10, 0 }, { "PAD9", 48, 10, 0 }, { "PAD10", 47, 10, 0 }, { "PAD11", 45, 10, 0 }, { "PAD12", 43, 10, 0 }, { "PAD13", 49, 10, 0 }, { "PAD14", 55, 10, 0 }, { "PAD15", 51, 10, 0 }, { "PAD16", 53, 10, 0 },
    { "QLINK1-TURN", 16, 1, 1 }, { "QLINK2-TURN", 17, 1, 1 }, { "QLINK3-TURN", 18, 1, 1 }, { "QLINK4-TURN", 19, 1, 1 },
    { "WHEEL", 100, 1, 1 },
};

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;   // the profile file and learn: one at a time
static char file[400];
static char status[640] = "not looked for yet";

void buttons_init(const char *f) { snprintf(file, sizeof file, "%s", f ? f : ""); }

int buttons_norm(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == ' ' || c == '_') c = '-';
        else if (isalnum(c)) c = (unsigned char)toupper(c);
        else if (c != '-') return -1;
        if (k + 1 >= n || k >= sizeof(((struct button *)0)->name) - 1) return -1;
        out[k++] = (char)c;
    }
    out[k] = 0;
    return k ? 0 : -1;
}

int buttons_parse_line(const char *line, struct button *b)
{
    char nm[64];
    const char *eq = strchr(line, '=');
    if (!eq) return -1;
    size_t l = (size_t)(eq - line);
    while (l && isspace((unsigned char)line[l - 1])) l--;
    while (l && isspace((unsigned char)*line)) { line++; l--; }
    if (!l || l >= sizeof nm) return -1;
    memcpy(nm, line, l);
    nm[l] = 0;
    char *end;
    const char *v = eq + 1;
    while (isspace((unsigned char)*v)) v++;
    int cc = (v[0] == 'c' || v[0] == 'C') && (v[1] == 'c' || v[1] == 'C');
    if (cc) v += 2;
    long note = strtol(v, &end, 10);
    if (end == v || note < 0 || note > 127) return -1;
    long ch = 1;
    while (isspace((unsigned char)*end)) end++;
    if (*end && *end != '#') {
        char *e2;
        ch = strtol(end, &e2, 10);
        if (e2 == end || ch < 1 || ch > 16) return -1;
        end = e2;
        while (isspace((unsigned char)*end)) end++;
        if (*end && *end != '#') return -1;
    }
    memset(b, 0, sizeof *b);
    if (buttons_norm(nm, b->name, sizeof b->name)) return -1;
    b->note = (int)note; b->channel = (int)ch; b->learned = 1; b->cc = cc;
    return 0;
}

static void put(struct button *t, int *n, const struct button *b)
{
    for (int i = 0; i < *n; i++)
        if (!strcmp(t[i].name, b->name)) { t[i] = *b; return; }   // the user's entry wins over the built-in one
    if (*n < MAXB) t[(*n)++] = *b;
}

static int load_locked(struct button *t, int max)
{
    int n = 0, c, p;
    if (max > MAXB) max = MAXB;
    char label[160];
    int force = midi_find_port("Akai Pro Force", NULL, 0, &c, &p, label, sizeof label) == 0;
    if (force)
        for (size_t i = 0; i < sizeof FORCE / sizeof *FORCE; i++) {
            struct button b = { .note = FORCE[i].note, .channel = 1 };
            snprintf(b.name, sizeof b.name, "%s", FORCE[i].name);
            put(t, &n, &b);
        }
    int sample = midi_find_port("MPC Sample", NULL, 0, &c, &p, label, sizeof label) == 0;
    if (sample)
        for (size_t i = 0; i < sizeof SAMPLE / sizeof *SAMPLE; i++) {
            struct button b = { .note = SAMPLE[i].note, .channel = 1 };
            snprintf(b.name, sizeof b.name, "%s", SAMPLE[i].name);
            put(t, &n, &b);
        }
    if (!force && !sample && midi_find_port("MPC LIVE III", NULL, 0, &c, &p, NULL, 0) == 0)
        for (size_t i = 0; i < sizeof LIVE3 / sizeof *LIVE3; i++) {
            struct button b = { .note = LIVE3[i].note, .channel = LIVE3[i].channel, .cc = LIVE3[i].cc };
            snprintf(b.name, sizeof b.name, "%s", LIVE3[i].name);
            put(t, &n, &b);
        }
    int builtin = n;
    FILE *f = file[0] ? fopen(file, "r") : NULL;
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            struct button b;
            if (!buttons_parse_line(line, &b)) put(t, &n, &b);
        }
        fclose(f);
    }
    if (midi_find_port(NULL, "Virtual RawMIDI", 1, &c, &p, label, sizeof label)) {
        if (!try_inject())
            snprintf(status, sizeof status, "Sent to %s. %d built-in, %d learned (%s).", inject_path, builtin, n - builtin,
                     file[0] ? file : "no profile file");
        else
            snprintf(status, sizeof status, "MPC's button input wasn't found (no \"Virtual RawMIDI\" port or inject device).");
    } else if (!n)
        snprintf(status, sizeof status, "Sent to %s. No profile for this device yet: use learn_button to record each button.", label);
    else
        snprintf(status, sizeof status, "Sent to %s. %d built-in, %d learned (%s).", label, builtin, n - builtin,
                 file[0] ? file : "no profile file");
    return n < max ? n : max;
}

int buttons_list(struct button *out, int max)
{
    struct button t[MAXB];
    pthread_mutex_lock(&mtx);
    int n = load_locked(t, MAXB);
    pthread_mutex_unlock(&mtx);
    if (n > max) n = max;
    memcpy(out, t, (size_t)n * sizeof *t);
    return n;
}

const char *buttons_status(void) { return status; }

static int find(const struct button *t, int n, const char *norm)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].name, norm)) return i;
    return -1;
}

static void sleep_ms(int ms) { nanosleep(&(struct timespec){ ms / 1000, (long)(ms % 1000) * 1000000L }, NULL); }

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Buttons held down by buttons_set: released by the watchdog when nobody has said "down" again for 4 s.
#define HOLDS 8
static struct { int on, c, p, note, ch; long long until; } held[HOLDS];
static pthread_mutex_t hmtx = PTHREAD_MUTEX_INITIALIZER;
static int watchdog_started;

static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        sleep_ms(200);
        pthread_mutex_lock(&hmtx);
        for (int i = 0; i < HOLDS; i++)
            if (held[i].on && now_ms() > held[i].until) {
                uint8_t off[3] = { (uint8_t)(0x90 | (held[i].ch - 1)), (uint8_t)held[i].note, 0 };
                send_btn(held[i].c, held[i].p, off);
                held[i].on = 0;
            }
        pthread_mutex_unlock(&hmtx);
    }
    return NULL;
}

static void start_watchdog(void)   // as the addin's other threads: every signal blocked, a small stack
{
    if (watchdog_started) return;
    sigset_t all, old;
    pthread_attr_t at;
    pthread_t th;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&th, &at, watchdog, NULL)) watchdog_started = 1;
    pthread_attr_destroy(&at);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

// The profile entry and MPC's port for a name. 0 on success, else err says why.
static int resolve(const char *name, struct button *out, int *c, int *p, char *err, size_t en)
{
    char norm[24];
    struct button t[MAXB];
    if (buttons_norm(name, norm, sizeof norm)) { snprintf(err, en, "\"%s\" isn't a button name (letters, digits, - only).", name); return -1; }
    pthread_mutex_lock(&mtx);
    int n = load_locked(t, MAXB);
    pthread_mutex_unlock(&mtx);
    int i = find(t, n, norm);
    if (i < 0) { snprintf(err, en, "No button called %s. %s", norm, n ? "list_buttons shows them." : buttons_status()); return -1; }
    if (midi_find_port(NULL, "Virtual RawMIDI", 1, c, p, NULL, 0)) {
        if (try_inject()) { snprintf(err, en, "%s", buttons_status()); return -1; }
        *c = -1; *p = -1;
    }
    *out = t[i];
    return 0;
}

int buttons_set(const char *name, int down, char *err, size_t en)
{
    struct button b;
    int c, p;
    if (resolve(name, &b, &c, &p, err, en)) return -1;
    if (b.cc) { snprintf(err, en, "%s is a knob: turn it with turn_knob.", b.name); return -1; }
    uint8_t m[3] = { (uint8_t)(0x90 | (b.channel - 1)), (uint8_t)b.note, (uint8_t)(down ? 127 : 0) };
    pthread_mutex_lock(&hmtx);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < HOLDS; i++) {
        if (held[i].on && held[i].note == b.note && held[i].ch == b.channel) slot = i;
        if (!held[i].on && free_slot < 0) free_slot = i;
    }
    if (down && slot < 0 && free_slot < 0) { pthread_mutex_unlock(&hmtx); snprintf(err, en, "Too many buttons are held down at once."); return -1; }
    if (down) {
        if (slot < 0 && send_btn(c, p, m)) { pthread_mutex_unlock(&hmtx); snprintf(err, en, "Sending to MPC failed: %s.", midi_error()); return -1; }
        if (slot < 0) slot = free_slot;
        held[slot].on = 1; held[slot].c = c; held[slot].p = p; held[slot].note = b.note; held[slot].ch = b.channel;
        held[slot].until = now_ms() + 4000;
        start_watchdog();
    } else {
        if (slot >= 0) held[slot].on = 0;
        send_btn(c, p, m);
    }
    pthread_mutex_unlock(&hmtx);
    return 0;
}

int buttons_press(const char *name, int hold_ms, char *err, size_t en)
{
    struct button b;
    int c, p;
    if (resolve(name, &b, &c, &p, err, en)) return -1;
    if (b.cc) { snprintf(err, en, "%s is a knob: turn it with turn_knob.", b.name); return -1; }
    uint8_t on[3] = { (uint8_t)(0x90 | (b.channel - 1)), (uint8_t)b.note, 127 };
    uint8_t off[3] = { on[0], on[1], 0 };   // the hardware releases with a note on of velocity 0
    if (send_btn(c, p, on)) { snprintf(err, en, "Sending to MPC failed: %s.", midi_error()); return -1; }
    sleep_ms(hold_ms);
    send_btn(c, p, off);
    return 0;
}

int buttons_dial(int delta, char *err, size_t en)
{
    int c, p;
    if (midi_find_port(NULL, "Virtual RawMIDI", 1, &c, &p, NULL, 0)) {
        if (try_inject()) { snprintf(err, en, "%s", buttons_status()); return -1; }
        c = -1; p = -1;
    }
    if (delta == 0) return 0;
    int steps = delta < 0 ? -delta : delta;
    if (steps > 63) steps = 63;
    uint8_t val = delta > 0 ? (uint8_t)steps : (uint8_t)(128 - steps);
    uint8_t m[3] = { 0xB0, 0x64, val };
    if (send_btn(c, p, m)) { snprintf(err, en, "Sending dial CC failed: %s.", midi_error()); return -1; }
    return 0;
}

int buttons_turn(const char *name, int steps, char *err, size_t en)
{
    struct button b;
    int c, p;
    if (!steps || steps > 64 || steps < -64) { snprintf(err, en, "steps is 1 to 64 clockwise, or -1 to -64 counter-clockwise."); return -1; }
    if (resolve(name, &b, &c, &p, err, en)) return -1;
    if (!b.cc) { snprintf(err, en, "%s is a button, not a knob: use press_button.", b.name); return -1; }
    uint8_t m[3] = { (uint8_t)(0xB0 | (b.channel - 1)), (uint8_t)b.note, (uint8_t)(steps > 0 ? 1 : 127) };
    for (int i = 0, n = steps > 0 ? steps : -steps; i < n; i++) {
        if (send_note(c, p, m)) { snprintf(err, en, "Sending to MPC failed: %s.", c < 0 ? "its button input is busy or closed" : midi_error()); return -1; }
        sleep_ms(8);   // the hardware's own clicks are spaced like this; the input queue holds 64
    }
    return 0;
}

// Rewrite the profile file with name=note ch, replacing a line with the same name.
static int save(const char *norm, int note, int ch)
{
    if (!file[0]) return -1;
    char tmp[420];
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *o = fopen(tmp, "w");
    if (!o) return -1;
    FILE *f = fopen(file, "r");
    char line[160];
    int wrote_header = 0;
    while (f && fgets(line, sizeof line, f)) {
        struct button b;
        if (!buttons_parse_line(line, &b) && !strcmp(b.name, norm)) continue;
        fputs(line, o);
        wrote_header = 1;
    }
    if (f) fclose(f);
    if (!wrote_header) fputs("# Button profile: NAME=note [channel]. \"learn_button\" adds lines here; edit freely.\n", o);
    fprintf(o, "%s=%d %d\n", norm, note, ch);
    int bad = ferror(o);
    if (fclose(o) || bad) { unlink(tmp); return -1; }
    return rename(tmp, file);
}

int buttons_learn(const char *name, int timeout_ms, int *note, int *channel, char *err, size_t en)
{
    char norm[24];
    int sc = 0, sp = 0;
    if (buttons_norm(name, norm, sizeof norm)) { snprintf(err, en, "\"%s\" isn't a button name (letters, digits, - only).", name); return -1; }
    if (!file[0]) { snprintf(err, en, "There is nowhere to save a profile (no folder for buttons.conf)."); return -1; }
    int tapped = surface_active();   // MPC holds the port exclusively: listen from inside its reads instead
    if (!tapped && midi_find_port(NULL, "Private", 0, &sc, &sp, NULL, 0)) {
        snprintf(err, en, "The device's own button port (\"... Private\") wasn't found, so there is nothing to learn from.");
        return -1;
    }
    pthread_mutex_lock(&mtx);
    int rc = -1, got = 0, left = timeout_ms;
    if (tapped) {
        unsigned long pos = surface_tap_pos();
        while (left > 0 && !got) {
            uint8_t m[3];
            int k = surface_tap_next(&pos, m);
            if (k > 0 && (m[0] & 0xF0) == 0x90 && m[2] > 0) { *note = m[1]; *channel = (m[0] & 15) + 1; got = 1; }
            else if (k == 0) { sleep_ms(10); left -= 10; }
        }
    } else {
        if (midi_watch(sc, sp, 1)) { snprintf(err, en, "Couldn't listen to the device's buttons: %s.", midi_error()); goto out; }
        midi_drain();
        struct midi_msg m[64];
        while (left > 0 && !got) {
            int slice = left < 250 ? left : 250, k = midi_listen(slice, m, 64);
            left -= slice;
            for (int i = 0; i < k; i++)
                if ((m[i].b[0] & 0xF0) == 0x90 && m[i].b[2] > 0 && m[i].len == 3) {
                    *note = m[i].b[1]; *channel = (m[i].b[0] & 15) + 1; got = 1;
                    break;
                }
        }
        midi_watch(sc, sp, 0);
    }
    if (!got) { snprintf(err, en, "No button was pressed in %d ms.", timeout_ms); goto out; }
    if (save(norm, *note, *channel)) { snprintf(err, en, "Heard note %d but couldn't write %s.", *note, file); goto out; }
    rc = 0;
out:
    pthread_mutex_unlock(&mtx);
    return rc;
}

// The profile's name for a note (kind 8 or 9) or a controller (kind 0xB) on a channel, "" for none.
static const char *profile_name(const struct button *t, int n, int kind, int num, int ch)
{
    for (int i = 0; i < n; i++)
        if (t[i].note == num && t[i].channel == ch && t[i].cc == (kind == 0xB)) return t[i].name;
    return "";
}

int buttons_listen(int ms, struct button_event *out, int max, char *err, size_t en)
{
    if (!surface_active()) { snprintf(err, en, "MPC hasn't opened its button input through the addin, so there is nothing to listen to."); return -1; }
    struct button prof[MAXB];
    pthread_mutex_lock(&mtx);
    int np = load_locked(prof, MAXB);
    pthread_mutex_unlock(&mtx);
    unsigned long pos = surface_tap_pos();
    int n = 0;
    for (int left = ms; left > 0; ) {
        uint8_t m[3];
        int k = surface_tap_next(&pos, m);
        if (k == 0) { sleep_ms(10); left -= 10; continue; }
        if (k < 0 || n >= max) continue;
        out[n].kind = m[0] >> 4;   // 8 note off, 9 note on, 0xB controller, 0xE pitch bend ...
        out[n].on = out[n].kind == 9 && m[2] > 0;
        out[n].note = m[1]; out[n].channel = (m[0] & 15) + 1; out[n].velocity = m[2];
        snprintf(out[n].match, sizeof out[n].match, "%s",
                 out[n].kind == 8 || out[n].kind == 9 || out[n].kind == 0xB ? profile_name(prof, np, out[n].kind, m[1], out[n].channel) : "");
        n++;
    }
    return n;
}
