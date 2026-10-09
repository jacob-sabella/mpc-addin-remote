#define _GNU_SOURCE
#include "buttons.h"
#include "midi.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXB 160

// The Akai Force: its buttons are MIDI notes on channel 1 (measured on a Force: press = velocity 127,
// release = velocity 0, on the MIDI port "Akai Pro Force Private").
static const struct { const char *name; int note; } FORCE[] = {
    { "KNOBS", 1 }, { "MENU", 2 }, { "MATRIX", 3 }, { "NOTE", 4 }, { "CLIP", 9 }, { "MIXER", 11 }, { "SAVE", 36 },
    { "EDIT", 37 }, { "SHIFT", 49 }, { "UNDO", 67 }, { "REC", 73 }, { "STOP", 81 }, { "PLAY", 82 }, { "MUTE", 91 },
    { "SOLO", 92 }, { "ARM", 93 }, { "UP", 112 }, { "DOWN", 113 }, { "LEFT", 114 }, { "RIGHT", 115 },
    { "LAUNCH", 116 }, { "COPY", 122 },
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
    long note = strtol(eq + 1, &end, 10);
    if (end == eq + 1 || note < 0 || note > 127) return -1;
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
    b->note = (int)note; b->channel = (int)ch; b->learned = 1;
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
    if (midi_find_port(NULL, "Virtual RawMIDI", 1, &c, &p, label, sizeof label))
        snprintf(status, sizeof status, "MPC's button input (a \"Virtual RawMIDI\" port) wasn't found: this device can't be pressed from here.");
    else if (!n)
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

int buttons_press(const char *name, int hold_ms, char *err, size_t en)
{
    char norm[24];
    struct button t[MAXB];
    if (buttons_norm(name, norm, sizeof norm)) { snprintf(err, en, "\"%s\" isn't a button name (letters, digits, - only).", name); return -1; }
    pthread_mutex_lock(&mtx);
    int n = load_locked(t, MAXB);
    pthread_mutex_unlock(&mtx);
    int i = find(t, n, norm), c, p;
    if (i < 0) { snprintf(err, en, "No button called %s. %s", norm, n ? "list_buttons shows them." : buttons_status()); return -1; }
    if (midi_find_port(NULL, "Virtual RawMIDI", 1, &c, &p, NULL, 0)) { snprintf(err, en, "%s", buttons_status()); return -1; }
    uint8_t on[3] = { (uint8_t)(0x90 | (t[i].channel - 1)), (uint8_t)t[i].note, 127 };
    uint8_t off[3] = { on[0], on[1], 0 };   // the hardware releases with a note on of velocity 0
    if (midi_send_to(c, p, on)) { snprintf(err, en, "Sending to MPC failed: %s.", midi_error()); return -1; }
    sleep_ms(hold_ms);
    midi_send_to(c, p, off);
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
    int sc, sp;
    if (buttons_norm(name, norm, sizeof norm)) { snprintf(err, en, "\"%s\" isn't a button name (letters, digits, - only).", name); return -1; }
    if (!file[0]) { snprintf(err, en, "There is nowhere to save a profile (no folder for buttons.conf)."); return -1; }
    if (midi_find_port(NULL, "Private", 0, &sc, &sp, NULL, 0)) {
        snprintf(err, en, "The device's own button port (\"... Private\") wasn't found, so there is nothing to learn from.");
        return -1;
    }
    pthread_mutex_lock(&mtx);
    int rc = -1;
    if (midi_watch(sc, sp, 1)) { snprintf(err, en, "Couldn't listen to the device's buttons: %s.", midi_error()); goto out; }
    midi_drain();
    struct midi_msg m[64];
    int left = timeout_ms, got = 0;
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
    if (!got) { snprintf(err, en, "No button was pressed in %d ms.", timeout_ms); goto out; }
    if (save(norm, *note, *channel)) { snprintf(err, en, "Heard note %d but couldn't write %s.", *note, file); goto out; }
    rc = 0;
out:
    pthread_mutex_unlock(&mtx);
    return rc;
}
