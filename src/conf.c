#define _GNU_SOURCE
#include "conf.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void conf_defaults(struct conf *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    snprintf(c->bind, sizeof c->bind, "0.0.0.0");
    c->port = 8080;
    snprintf(c->touch_device, sizeof c->touch_device, "auto");
    c->touch_rotate = 90;     // the MPC Key 37's panel (verified); other models may need another value
    c->max_fps = 10;
    c->max_clients = 6;
    c->nice = 10;
}

static int to_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi) return -1;
    *out = (int)n;
    return 0;
}

int conf_line(struct conf *c, const char *line)
{
    char k[32], v[96];
    while (isspace((unsigned char)*line)) line++;
    if (!*line || *line == '#') return 0;
    if (sscanf(line, " %31[a-z_] = %95s", k, v) != 2) return -1;
    if (!strcmp(k, "enabled")) return to_int(v, 0, 1, &c->enabled);
    if (!strcmp(k, "port")) return to_int(v, 1, 65535, &c->port);
    if (!strcmp(k, "touch_rotate")) {
        int r;
        if (to_int(v, 0, 270, &r) || r % 90) return -1;
        c->touch_rotate = r;
        return 0;
    }
    if (!strcmp(k, "max_fps")) return to_int(v, 1, 60, &c->max_fps);
    if (!strcmp(k, "max_clients")) return to_int(v, 1, 32, &c->max_clients);
    if (!strcmp(k, "nice")) return to_int(v, 0, 19, &c->nice);
    if (!strcmp(k, "bind")) {
        size_t n = strlen(v);
        if (n >= sizeof c->bind) return -1;
        memcpy(c->bind, v, n + 1);
        return 0;
    }
    if (!strcmp(k, "touch_device")) {
        size_t n = strlen(v);
        if (n >= sizeof c->touch_device || (strcmp(v, "auto") && strncmp(v, "/dev/input/", 11))) return -1;
        memcpy(c->touch_device, v, n + 1);
        return 0;
    }
    return -1;
}

int conf_load(struct conf *c, const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    char line[256];
    int bad = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (conf_line(c, line)) bad++;
    }
    fclose(f);
    return bad;
}

int query_int(const char *q, const char *key, int def, int lo, int hi)
{
    size_t kl = strlen(key);
    while (q && *q) {
        const char *amp = strchr(q, '&');
        size_t n = amp ? (size_t)(amp - q) : strlen(q);
        if (n > kl && !strncmp(q, key, kl) && q[kl] == '=') {
            char v[16];
            size_t vl = n - kl - 1;
            if (vl == 0 || vl >= sizeof v) return def;
            memcpy(v, q + kl + 1, vl);
            v[vl] = 0;
            char *end;
            long x = strtol(v, &end, 10);
            if (*end) return def;
            return x < lo ? lo : x > hi ? hi : (int)x;
        }
        q = amp ? amp + 1 : NULL;
    }
    return def;
}
