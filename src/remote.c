// mpc-remote-addin: the MPC's screen and touchscreen over HTTP, and an MCP endpoint (/mcp, src/mcp.c) for models,
// from inside the MPC process (LD_PRELOAD).
// It starts only in the process whose executable is named MPC. The launch script and anything else that inherits
// LD_PRELOAD load it and do nothing. Everything runs on the addin's own threads, at normal scheduling and a low
// priority, with every signal blocked so MPC's signals still go to MPC's threads.
#define _GNU_SOURCE
#include "capture.h"
#include "conf.h"
#include "device.h"
#include "mcp.h"
#include "page.h"
#include "png.h"
#include "touch.h"
#include "version.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX_STREAMS 2

static struct conf C;
static atomic_int clients, streams, started;

#define LOG(...) fprintf(stderr, "mpc-remote-addin: " __VA_ARGS__)

// ---- where the .so is: the config sits next to it ----
static void addin_dir(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return;
    uintptr_t me = (uintptr_t)&addin_dir;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char *path = strchr(line, '/');
        if (!path || sscanf(line, "%lx-%lx", &a, &b) != 2 || me < a || me >= b) continue;
        path[strcspn(path, "\n")] = 0;
        char *slash = strrchr(path, '/');
        if (slash) *slash = 0;
        snprintf(out, n, "%s", path);
        break;
    }
    fclose(f);
}

static int is_mpc_process(void)
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    return !strcmp(base ? base + 1 : exe, "MPC");
}

// ---- HTTP ----
static int send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void reply_h(int fd, const char *status, const char *type, const char *extra, const void *body, size_t n)
{
    char h[320];
    int k = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "%sConnection: close\r\n\r\n", status, type, n, extra);
    if (send_all(fd, h, (size_t)k) == 0 && n) send_all(fd, body, n);
}

static void reply(int fd, const char *status, const char *type, const void *body, size_t n)
{
    reply_h(fd, status, type, "", body, n);
}

static void reply_text(int fd, const char *status, const char *text)
{
    reply(fd, status, "text/plain; charset=utf-8", text, strlen(text));
}

static atomic_int encode_ms;   // the last PNG encode's time

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void serve_info(int fd)
{
    int w = 0, h = 0, r = cap_size(&w, &h);
    char b[512];
    int n = snprintf(b, sizeof b,
                     "{\"version\":\"%s\",\"width\":%d,\"height\":%d,\"format\":\"%s\",\"capture\":\"%s\","
                     "\"touch\":\"%s\",\"screen_rotate\":%d,\"touch_rotate\":%d,\"png\":%s,"
                     "\"capture_ms\":%d,\"encode_ms\":%d}",
                     REMOTE_VERSION, w, h, cap_format(), r ? cap_error(r) : "ok", touch_device(), C.screen_rotate,
                     C.touch_rotate, png_init() ? "false" : "true", cap_last_ms(), atomic_load(&encode_ms));
    reply(fd, "200 OK", "application/json", b, (size_t)n);
}

// A frame as PNG into *png (malloc'd). 0 on success, else a capture error code (or 1: encoding failed).
// With last_crc, an unchanged frame returns 2 and no PNG.
static int grab_png(int scale, uint8_t **png, size_t *len, uint32_t *last_crc)
{
    int w, h;
    int r = cap_size(&w, &h);
    if (r) return r;
    size_t sz = (size_t)w * h * 3;
    uint8_t *rgb = malloc(sz);
    if (!rgb) return 1;
    int ow, oh;
    r = cap_frame(rgb, sz, scale, &ow, &oh);
    if (r) { free(rgb); return r; }
    if (last_crc) {
        uint32_t c = png_crc(rgb, (size_t)ow * oh * 3) ^ (uint32_t)scale;
        if (c == *last_crc) { free(rgb); return 2; }
        *last_crc = c;
    }
    long long t0 = now_ms();
    *png = png_encode(rgb, ow, oh, len);
    atomic_store(&encode_ms, (int)(now_ms() - t0));
    free(rgb);
    return *png ? 0 : 1;
}

static void serve_screen(int fd, int scale)
{
    uint8_t *png = NULL;
    size_t len = 0;
    int r = png_init() ? 1 : grab_png(scale, &png, &len, NULL);
    if (r == 0) reply(fd, "200 OK", "image/png", png, len);
    else reply_text(fd, "503 Service Unavailable", r < 0 ? cap_error(r) : "PNG encoding failed (no libz.so.1?)");
    free(png);
}

// The client is gone (a read would return end of file, or the socket failed).
static int peer_closed(int fd)
{
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

// multipart/x-mixed-replace PNG frames until the client leaves; an unchanged screen sends nothing more once its frame
// has gone out twice.
static void serve_stream(int fd, int scale, int fps)
{
    if (atomic_fetch_add(&streams, 1) >= MAX_STREAMS) {
        atomic_fetch_sub(&streams, 1);
        reply_text(fd, "503 Service Unavailable", "too many streams");
        return;
    }
    if (png_init()) {
        atomic_fetch_sub(&streams, 1);
        reply_text(fd, "503 Service Unavailable", "PNG encoding failed (no libz.so.1?)");
        return;
    }
    static const char hdr[] = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=mpcframe\r\n"
                              "Cache-Control: no-store\r\nConnection: close\r\n\r\n--mpcframe\r\n";
    uint32_t crc = 0;
    long long frame_ms = 1000 / fps;
    uint8_t *last = NULL;      // the newest frame, kept to send once more
    size_t last_len = 0;
    int again = 0;             // `last` has gone out once only
    if (send_all(fd, hdr, sizeof hdr - 1) == 0) {
        for (;;) {
            long long t0 = now_ms();
            uint8_t *png = NULL;
            size_t len = 0;
            int r = grab_png(scale, &png, &len, &crc);
            if (r == 0) {
                free(last);
                last = png, last_len = len, again = 1;
            }
            if (r == 0 || (r == 2 && again)) {
                // A browser shows a part only once the next part arrives, so the newest frame goes out a second time
                // when the screen stops changing: otherwise the page shows the screen one change late (blank at first).
                if (r == 2) again = 0;
                char part[96];
                int k = snprintf(part, sizeof part, "Content-Type: image/png\r\nContent-Length: %zu\r\n\r\n", last_len);
                static const char end[] = "\r\n--mpcframe\r\n";
                if (send_all(fd, part, (size_t)k) || send_all(fd, last, last_len) || send_all(fd, end, sizeof end - 1)) break;
            } else if (peer_closed(fd)) {
                break;
            }
            long long wait = (r < 0 ? 500 : frame_ms) - (now_ms() - t0);   // no display: look again in 0.5 s
            if (wait > 0) usleep((useconds_t)wait * 1000);
        }
    }
    free(last);
    atomic_fetch_sub(&streams, 1);
}

// Read the request head (up to the blank line), at most n-1 bytes, with the socket's receive timeout. *got is every
// byte read, which may run on into the body.
static int read_head(int fd, char *buf, size_t n, size_t *got)
{
    *got = 0;
    while (*got < n - 1) {
        ssize_t r = recv(fd, buf + *got, n - 1 - *got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        *got += (size_t)r;
        buf[*got] = 0;
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) return 0;
    }
    buf[*got] = 0;
    return *got && strchr(buf, '\n') ? 0 : -1;
}

// A request header's value (name case-insensitive), trimmed, into out; 0 if found.
static int header(const char *req, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    const char *p = strchr(req, '\n');
    while (p && p[1] && p[1] != '\r' && p[1] != '\n') {
        p++;
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1, *e = eol;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && isspace((unsigned char)e[-1])) e--;
            size_t l = (size_t)(e - v) < n - 1 ? (size_t)(e - v) : n - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 0;
        }
        p = *eol ? eol : NULL;
    }
    return -1;
}

#define MAX_BODY 65536

// A Host header naming the device the way a person does: an IP address, localhost, or a .local (mDNS) name. A DNS
// rebinding attack reaches the device under the attacker's own name, which is none of these.
static int host_is_direct(const char *host)
{
    char h[256];
    snprintf(h, sizeof h, "%s", host);
    char *p = h;
    if (*p == '[') {                                   // [v6]:port
        char *e = strchr(p, ']');
        if (!e) return 0;
        *e = 0;
        p++;
    } else {
        char *c = strrchr(p, ':');
        if (c) *c = 0;
    }
    struct in6_addr a6;
    struct in_addr a4;
    size_t l = strlen(p);
    return inet_pton(AF_INET, p, &a4) == 1 || inet_pton(AF_INET6, p, &a6) == 1 || !strcasecmp(p, "localhost")
           || (l > 6 && !strcasecmp(p + l - 6, ".local"));
}

// POST /mcp: the body (Content-Length bytes; part may have come with the head) to mcp_handle(), and its reply.
static void serve_mcp(int fd, const char *req, const char *rest, size_t have)
{
    char v[256], host[256];
    if (header(req, "Origin", v, sizeof v) == 0 && strcmp(v, "null")) {
        // a web page's request: only a page served from this same address may make one (no cross-site requests),
        // and only under a direct address (no DNS rebinding). Programs send no Origin.
        const char *o = strstr(v, "://");
        if (header(req, "Host", host, sizeof host) || !o || strcasecmp(o + 3, host) || !host_is_direct(host)) {
            reply_text(fd, "403 Forbidden", "cross-origin requests are refused");
            return;
        }
    }
    if (header(req, "Transfer-Encoding", v, sizeof v) == 0) { reply_text(fd, "411 Length Required", "send a Content-Length"); return; }
    if (header(req, "Content-Length", v, sizeof v)) { reply_text(fd, "411 Length Required", "Content-Length is needed"); return; }
    char *end;
    long len = strtol(v, &end, 10);
    if (*end || len < 0) { reply_text(fd, "400 Bad Request", "bad Content-Length"); return; }
    if (len > MAX_BODY) { reply_text(fd, "413 Payload Too Large", "at most 64 KB"); return; }
    char *body = malloc((size_t)len + 1);
    if (!body) { reply_text(fd, "503 Service Unavailable", "out of memory"); return; }
    size_t got = have < (size_t)len ? have : (size_t)len;
    memcpy(body, rest, got);
    while (got < (size_t)len) {
        ssize_t r = recv(fd, body + got, (size_t)len - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got < (size_t)len) { free(body); reply_text(fd, "400 Bad Request", "the body ended early"); return; }
    body[len] = 0;
    char *out;
    size_t outlen;
    int st = mcp_handle(body, (size_t)len, &out, &outlen);
    free(body);
    if (st == 202) reply(fd, "202 Accepted", "text/plain", "", 0);
    else if (!out) reply_text(fd, "500 Internal Server Error", "out of memory");
    else reply(fd, st == 200 ? "200 OK" : "400 Bad Request", "application/json", out, outlen);
    free(out);
}

static void handle(int fd)
{
    char req[8192], method[8], target[512];
    size_t got;
    if (read_head(fd, req, sizeof req, &got) || sscanf(req, "%7s %511s", method, target) != 2
        || strcspn(req + strlen(method) + 1, " \r\n") > 511) {       // a longer target would be cut short
        reply_text(fd, "400 Bad Request", "bad request");
        return;
    }
    if (!strcmp(target, "/mcp") && C.mcp) {
        const char *eoh = strstr(req, "\r\n\r\n"), *lf = strstr(req, "\n\n");
        const char *body = eoh ? eoh + 4 : lf ? lf + 2 : req + got;     // no blank line (timed out): no body yet
        if (!strcmp(method, "POST")) serve_mcp(fd, req, body, got - (size_t)(body - req));
        else reply_h(fd, "405 Method Not Allowed", "text/plain", "Allow: POST\r\n", "POST JSON-RPC here", 18);
        return;
    }
    if (strcmp(method, "GET")) {
        reply_text(fd, "405 Method Not Allowed", "GET only");
        return;
    }
    char *q = strchr(target, '?');
    if (q) *q++ = 0;
    const char *path = target;
    int sx, sy, sw, sh;

    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        reply(fd, "200 OK", "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
    } else if (!strcmp(path, "/info")) {
        serve_info(fd);
    } else if (!strcmp(path, "/screen.png") || !strcmp(path, "/screen")) {
        serve_screen(fd, query_int(q, "half", 0, 0, 1) ? 2 : 1);
    } else if (!strcmp(path, "/stream")) {
        serve_stream(fd, query_int(q, "half", 0, 0, 1) ? 2 : 1, query_int(q, "fps", C.max_fps, 1, C.max_fps));
    } else if (!strcmp(path, "/tap") || !strcmp(path, "/down") || !strcmp(path, "/move")) {
        int x = query_int(q, "x", -1, -1, 65535), y = query_int(q, "y", -1, -1, 65535);
        if (x < 0 || y < 0) { reply_text(fd, "400 Bad Request", "x and y are needed"); return; }
        int r = cap_to_scanout(x, y, &sx, &sy, &sw, &sh);   // the touch panel is in the scanout's frame
        if (r) { reply_text(fd, "503 Service Unavailable", cap_error(r)); return; }
        if (!strcmp(path, "/down")) touch_down(sx, sy, sw, sh);
        else if (!strcmp(path, "/move")) touch_move(sx, sy, sw, sh);
        else {
            gesture_lock();
            touch_down(sx, sy, sw, sh);
            usleep((useconds_t)query_int(q, "hold", 90, 10, 4000) * 1000);
            touch_up();
            gesture_unlock();
        }
        reply_text(fd, "200 OK", "ok");
    } else if (!strcmp(path, "/up")) {
        touch_up();
        reply_text(fd, "200 OK", "ok");
    } else {
        reply_text(fd, "404 Not Found", "not found");
    }
}

static void lower_priority(void)
{
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), C.nice);   // nice is per thread on Linux
}

// Close after the reply: unread request bytes would turn the close into a reset that can discard the reply.
static void finish(int fd)
{
    char junk[512];
    shutdown(fd, SHUT_WR);
    while (recv(fd, junk, sizeof junk, MSG_DONTWAIT) > 0) { }
    close(fd);
}

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    handle(fd);
    finish(fd);
    atomic_fetch_sub(&clients, 1);
    return NULL;
}

// Threads: normal scheduling (never MPC's real-time policy, whatever the creating thread has), detached.
static int spawn(void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_OTHER);
    struct sched_param sp = { .sched_priority = 0 };
    pthread_attr_setschedparam(&a, &sp);
    pthread_attr_setstacksize(&a, 256 * 1024);
    pthread_t t;
    int r = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r;
}

static void *server_thread(void *arg)
{
    (void)arg;
    lower_priority();
    if (touch_open(C.touch_device, C.touch_rotate)) LOG("no touchscreen found (%s): touch is off\n", C.touch_device);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)C.port) };
    if (inet_pton(AF_INET, C.bind, &sa.sin_addr) != 1) { LOG("bad bind address %s\n", C.bind); return NULL; }
    int s = -1, warned = 0;
    for (;;) {                    // the port may still be held (a previous MPC, another tool): keep trying
        s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        if (s >= 0) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (s >= 0 && bind(s, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(s, 16) == 0) break;
        if (!warned++) LOG("can't listen on %s:%d (%s); retrying every 10 s\n", C.bind, C.port, strerror(errno));
        if (s >= 0) close(s);
        sleep(10);
    }
    LOG("%s on http://%s:%d/ (MCP: %s)\n", REMOTE_VERSION, C.bind, C.port, C.mcp ? "/mcp" : "off");
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno != EINTR && errno != ECONNABORTED) usleep(100000);
            continue;
        }
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (atomic_fetch_add(&clients, 1) >= C.max_clients) {
            atomic_fetch_sub(&clients, 1);
            reply_text(c, "503 Service Unavailable", "busy");
            finish(c);
            continue;
        }
        if (spawn(conn_thread, (void *)(intptr_t)c)) {
            atomic_fetch_sub(&clients, 1);
            close(c);
        }
    }
    return NULL;
}

// Start the server (once). The constructor calls it inside MPC; a test harness or a stand-alone runner can call
// it directly. Returns 0 when the server thread is running or already was, -1 when disabled or it failed.
__attribute__((visibility("default"))) int mpc_remote_addin_start(void)
{
    if (atomic_exchange(&started, 1)) return 0;
    conf_defaults(&C);
    char path[512];
    const char *env = getenv("MPC_REMOTE_ADDIN_CONF");
    if (env) snprintf(path, sizeof path, "%s", env);
    else {
        char dir[400];
        addin_dir(dir, sizeof dir);
        snprintf(path, sizeof path, "%s/mpc_remote_addin.conf", dir[0] ? dir : ".");
    }
    int bad = conf_load(&C, path);
    if (bad) LOG("%d line(s) of %s not understood, ignored\n", bad, path);
    if (!C.enabled) { LOG("disabled in %s\n", path); return -1; }
    if (files_set_roots(C.mcp_files)) { LOG("mcp_files=%s not understood: MCP file access is off\n", C.mcp_files); files_set_roots("none"); }
    cap_set_rotation(C.screen_rotate);
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);   // the new thread (and every thread it creates) blocks all signals
    int r = spawn(server_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (r) { LOG("can't start: %s\n", strerror(r)); return -1; }
    return 0;
}

__attribute__((constructor)) static void addin_init(void)
{
    if (getenv("MPC_REMOTE_ADDIN_DISABLE") || !is_mpc_process()) return;
    mpc_remote_addin_start();
}
