#define _GNU_SOURCE
#include "device.h"
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <ifaddrs.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

// ---- status ----
static int read_text(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    size_t k = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[k] = 0;
    return 0;
}

static long long meminfo(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    return p ? atoll(p + strlen(key)) : -1;
}

static int cpu_total(unsigned long long *busy, unsigned long long *all)
{
    char b[256];
    if (read_text("/proc/stat", b, sizeof b)) return -1;
    unsigned long long v[8] = { 0 };
    if (sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
        return -1;
    *all = 0;
    for (int i = 0; i < 8; i++) *all += v[i];
    *busy = *all - v[3] - v[4];   // idle and iowait
    return 0;
}

static long long self_ticks(void)
{
    char b[1024];
    if (read_text("/proc/self/stat", b, sizeof b)) return -1;
    char *p = strrchr(b, ')');   // the name may hold spaces; the fields after it don't
    if (!p) return -1;
    unsigned long long ut, st;
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return -1;
    return (long long)(ut + st);
}

static void human(char *out, size_t n, double bytes)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    while (bytes >= 1024 && i < 4) { bytes /= 1024; i++; }
    snprintf(out, n, i ? "%.1f %s" : "%.0f %s", bytes, u[i]);
}

void dev_status(struct sb *b)
{
    char buf[4096], h1[32], h2[32];
    unsigned long long b0 = 0, a0 = 0, b1 = 0, a1 = 0;
    int have_cpu = cpu_total(&b0, &a0) == 0;
    long long t0 = self_ticks();
    nanosleep(&(struct timespec){ 0, 250000000 }, NULL);
    have_cpu = have_cpu && cpu_total(&b1, &a1) == 0 && a1 > a0;
    long long t1 = self_ticks();

    struct utsname un;
    if (uname(&un) == 0) sb_printf(b, "Kernel: %s %s (%s)\n", un.sysname, un.release, un.machine);
    if (read_text("/etc/os-release", buf, sizeof buf) == 0) {
        char *p = strstr(buf, "PRETTY_NAME=");
        if (p) { p += 12; p[strcspn(p, "\n")] = 0; sb_printf(b, "OS: %s\n", p); }
    }
    char exe[256];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[k > 0 ? k : 0] = 0;
    if (read_text("/proc/uptime", buf, sizeof buf) == 0) {
        long up = atol(buf);
        sb_printf(b, "Up: %ldd %02ld:%02ld\n", up / 86400, up / 3600 % 24, up / 60 % 60);
    }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (read_text("/proc/loadavg", buf, sizeof buf) == 0) {
        buf[strcspn(buf, "\n")] = 0;
        sb_printf(b, "Load average: %s (%ld cores)\n", buf, cores);
    }
    if (have_cpu) sb_printf(b, "CPU busy: %.0f%% of all cores\n", 100.0 * (double)(b1 - b0) / (double)(a1 - a0));
    if (have_cpu && t0 >= 0 && t1 >= t0)
        sb_printf(b, "This process (%s): %.0f%% of one core\n", exe[0] ? exe : "?",
                  100.0 * (double)(t1 - t0) * (double)cores / (double)(a1 - a0));   // /proc/stat counts every core
    if (read_text("/proc/meminfo", buf, sizeof buf) == 0) {
        long long tot = meminfo(buf, "MemTotal:"), av = meminfo(buf, "MemAvailable:");
        if (tot > 0) {
            human(h1, sizeof h1, (double)av * 1024);
            human(h2, sizeof h2, (double)tot * 1024);
            sb_printf(b, "Memory: %s available of %s\n", h1, h2);
        }
    }
    if (read_text("/proc/self/status", buf, sizeof buf) == 0) {
        long long rss = meminfo(buf, "VmRSS:"), th = meminfo(buf, "Threads:");
        human(h1, sizeof h1, (double)rss * 1024);
        sb_printf(b, "This process: %s resident, %lld threads\n", h1, th);
    }
    for (int i = 0; i < 16; i++) {
        char p[96], type[64];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (read_text(p, buf, 32)) break;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", i);
        if (read_text(p, type, sizeof type)) snprintf(type, sizeof type, "zone %d", i);
        type[strcspn(type, "\n")] = 0;
        sb_printf(b, "Temperature %s: %.1f C\n", type, atof(buf) / 1000);
    }
    FILE *m = fopen("/proc/mounts", "re");
    if (m) {
        char dev[256], dir[256], fs[64];
        while (fscanf(m, "%255s %255s %63s %*[^\n]", dev, dir, fs) == 3) {
            if (strcmp(dir, "/") && strncmp(dir, "/media", 6) && strncmp(dir, "/data", 5) && strncmp(dir, "/sdcard", 7)
                && strncmp(dir, "/mnt", 4)) continue;
            struct statvfs v;
            if (statvfs(dir, &v) || !v.f_blocks) continue;
            human(h1, sizeof h1, (double)v.f_bavail * v.f_frsize);
            human(h2, sizeof h2, (double)v.f_blocks * v.f_frsize);
            sb_printf(b, "Storage %s (%s): %s free of %s%s\n", dir, fs, h1, h2, v.f_flag & ST_RDONLY ? ", read-only" : "");
        }
        fclose(m);
    }
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !strcmp(i->ifa_name, "lo")) continue;
            char a[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a);
            sb_printf(b, "Network %s: %s\n", i->ifa_name, a);
        }
        freeifaddrs(ifs);
    }
}

// ---- files ----
#define MAX_ROOTS 8
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static char roots[MAX_ROOTS][128];
static int nroots;

int files_set_roots(const char *csv)
{
    char tmp[MAX_ROOTS][128];
    int n = 0;
    if (strcmp(csv, "none")) {
        const char *p = csv;
        while (*p) {
            size_t l = strcspn(p, ",");
            if (!l || l >= sizeof tmp[0] || p[0] != '/' || n >= MAX_ROOTS) return -1;
            memcpy(tmp[n], p, l);
            tmp[n][l] = 0;
            while (l > 1 && tmp[n][l - 1] == '/') tmp[n][--l] = 0;
            n++;
            p += l + (p[l] == ',');
            if (p[-1] == ',' && !*p) return -1;
        }
    }
    pthread_mutex_lock(&mtx);
    memcpy(roots, tmp, sizeof tmp);
    nroots = n;
    pthread_mutex_unlock(&mtx);
    return 0;
}

void files_roots(struct sb *b)
{
    pthread_mutex_lock(&mtx);
    if (!nroots) sb_puts(b, "File access is off (mcp_files=none in the addin's settings).\n");
    for (int i = 0; i < nroots; i++) {
        struct stat st;
        sb_printf(b, "%s/%s\n", roots[i], stat(roots[i], &st) == 0 && S_ISDIR(st.st_mode) ? "" : "  (not present)");
    }
    pthread_mutex_unlock(&mtx);
}

// path, resolved (symbolic links and .. followed), into out, if it is a root or inside one.
static int resolve(const char *path, char *out, char *err, size_t en)
{
    char *r = realpath(path, NULL);
    if (!r) { snprintf(err, en, "%s: %s", path, strerror(errno)); return -1; }
    int ok = 0;
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < nroots && !ok; i++) {
        char *rr = realpath(roots[i], NULL);    // a root may itself be a link (/sdcard)
        const char *base = rr ? rr : roots[i];
        size_t l = strlen(base);
        ok = !strncmp(r, base, l) && (r[l] == 0 || r[l] == '/' || !strcmp(base, "/"));
        free(rr);
    }
    int none = !nroots;
    pthread_mutex_unlock(&mtx);
    if (!ok) {
        snprintf(err, en, none ? "file access is off (mcp_files=none)" : "%s is outside the folders files can be read from (list_files with no path shows them)", path);
        free(r);
        return -1;
    }
    snprintf(out, PATH_MAX, "%s", r);
    free(r);
    return 0;
}

struct ent { char *name; int dir; long long size; time_t mtime; };

static int ent_cmp(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

int files_list(const char *path, struct sb *b, char *err, size_t en)
{
    char real[PATH_MAX];
    if (resolve(path, real, err, en)) return -1;
    DIR *d = opendir(real);
    if (!d) { snprintf(err, en, "%s: %s", path, strerror(errno)); return -1; }
    struct ent *e = NULL;
    int n = 0, cap = 0, more = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n >= 500) { more++; continue; }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            struct ent *ne = realloc(e, (size_t)cap * sizeof *e);
            if (!ne) break;
            e = ne;
        }
        char p[PATH_MAX + 256];
        snprintf(p, sizeof p, "%s/%s", real, de->d_name);
        struct stat st;
        if (stat(p, &st)) memset(&st, 0, sizeof st);
        e[n].name = strdup(de->d_name);
        if (!e[n].name) break;
        e[n].dir = S_ISDIR(st.st_mode);
        e[n].size = st.st_size;
        e[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(e, (size_t)n, sizeof *e, ent_cmp);
    sb_printf(b, "%s: %d folder entries\n", real, n + more);
    for (int i = 0; i < n; i++) {
        char t[32], h[32];
        struct tm tm;
        strftime(t, sizeof t, "%Y-%m-%d %H:%M", localtime_r(&e[i].mtime, &tm));
        if (e[i].dir) sb_printf(b, "%s/\n", e[i].name);
        else { human(h, sizeof h, (double)e[i].size); sb_printf(b, "%s  (%s, %s)\n", e[i].name, h, t); }
        free(e[i].name);
    }
    if (more) sb_printf(b, "... and %d more\n", more);
    free(e);
    return 0;
}

int files_read(const char *path, long long offset, int max, struct sb *b, char *err, size_t en)
{
    char real[PATH_MAX];
    if (resolve(path, real, err, en)) return -1;
    struct stat st;
    if (stat(real, &st) || !S_ISREG(st.st_mode)) { snprintf(err, en, "%s isn't a file", path); return -1; }
    FILE *f = fopen(real, "re");
    if (!f) { snprintf(err, en, "%s: %s", path, strerror(errno)); return -1; }
    char *buf = malloc((size_t)max + 1);
    if (!buf) { fclose(f); snprintf(err, en, "out of memory"); return -1; }
    size_t k = 0;
    if (offset < 0) offset = 0;
    if (fseeko(f, (off_t)offset, SEEK_SET) == 0) k = fread(buf, 1, (size_t)max, f);
    fclose(f);
    if (memchr(buf, 0, k)) {
        snprintf(err, en, "%s looks binary (%lld bytes): only text files can be read", path, (long long)st.st_size);
        free(buf);
        return -1;
    }
    buf[k] = 0;
    long long end = offset + (long long)k;
    sb_printf(b, "%s: bytes %lld-%lld of %lld%s\n\n", real, offset, end, (long long)st.st_size,
              end < st.st_size ? " (more: read again with offset)" : "");
    sb_raw(b, buf, k);
    free(buf);
    return 0;
}

struct walk { const char *pat; int max, found, seen; struct sb *b; };

static void walk(struct walk *w, const char *dir, int depth)
{
    if (depth > 8 || w->found >= w->max || w->seen > 20000) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && w->found < w->max && w->seen++ <= 20000) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st)) continue;
        if (!fnmatch(w->pat, de->d_name, FNM_CASEFOLD)) {
            w->found++;
            sb_printf(w->b, "%s%s\n", p, S_ISDIR(st.st_mode) ? "/" : "");
        }
        if (S_ISDIR(st.st_mode)) walk(w, p, depth + 1);   // lstat: links to folders aren't followed
    }
    closedir(d);
}

int files_find(const char *path, const char *pattern, int max, struct sb *b, char *err, size_t en)
{
    char real[PATH_MAX];
    if (resolve(path, real, err, en)) return -1;
    struct sb found = { 0 };
    struct walk w = { .pat = pattern, .max = max, .b = &found };
    walk(&w, real, 0);
    sb_printf(b, "%d match(es) for \"%s\" under %s%s\n", w.found, pattern, real,
              w.found >= max ? " (stopped at the limit)" : w.seen > 20000 ? " (stopped after 20000 entries)" : "");
    if (found.n) sb_raw(b, found.p, found.n);
    sb_free(&found);
    return 0;
}
