// mpc-remote-addin: the MPC's screen and touchscreen over HTTP, from inside the MPC process (LD_PRELOAD).
// It starts only in the process whose executable is named MPC. The launch script and anything else that inherits
// LD_PRELOAD load it and do nothing. Everything runs on the addin's own threads, at normal scheduling and a low
// priority, with every signal blocked so MPC's signals still go to MPC's threads.
#define _GNU_SOURCE
#include "capture.h"
#include "conf.h"
#include "page.h"
#include "png.h"
#include "touch.h"
#include <arpa/inet.h>
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

#define VERSION "0.1.0"
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

static void reply(int fd, const char *status, const char *type, const void *body, size_t n)
{
    char h[256];
    int k = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", status, type, n);
    if (send_all(fd, h, (size_t)k) == 0 && n) send_all(fd, body, n);
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
                     VERSION, w, h, cap_format(), r ? cap_error(r) : "ok", touch_device(), C.screen_rotate,
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

// multipart/x-mixed-replace PNG frames until the client leaves; an unchanged screen sends nothing.
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
                              "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    uint32_t crc = 0;
    long long frame_ms = 1000 / fps;
    if (send_all(fd, hdr, sizeof hdr - 1) == 0) {
        for (;;) {
            long long t0 = now_ms();
            uint8_t *png = NULL;
            size_t len = 0;
            int r = grab_png(scale, &png, &len, &crc);
            if (r == 0) {
                char part[96];
                int k = snprintf(part, sizeof part, "--mpcframe\r\nContent-Type: image/png\r\nContent-Length: %zu\r\n\r\n", len);
                int bad = send_all(fd, part, (size_t)k) || send_all(fd, png, len) || send_all(fd, "\r\n", 2);
                free(png);
                if (bad) break;
            } else if (peer_closed(fd)) {
                break;
            }
            long long wait = (r < 0 ? 500 : frame_ms) - (now_ms() - t0);   // no display: look again in 0.5 s
            if (wait > 0) usleep((useconds_t)wait * 1000);
        }
    }
    atomic_fetch_sub(&streams, 1);
}

// Read the request head (up to the blank line), at most n-1 bytes, with the socket's receive timeout.
static int read_head(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n - 1) {
        ssize_t r = recv(fd, buf + got, n - 1 - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
        buf[got] = 0;
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) return 0;
    }
    buf[got] = 0;
    return got && strchr(buf, '\n') ? 0 : -1;
}

static void handle(int fd)
{
    char req[2048], method[8], target[512];
    if (read_head(fd, req, sizeof req) || sscanf(req, "%7s %511s", method, target) != 2) {
        reply_text(fd, "400 Bad Request", "bad request");
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
            touch_down(sx, sy, sw, sh);
            usleep((useconds_t)query_int(q, "hold", 90, 10, 4000) * 1000);
            touch_up();
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
    LOG("%s on http://%s:%d/\n", VERSION, C.bind, C.port);
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
