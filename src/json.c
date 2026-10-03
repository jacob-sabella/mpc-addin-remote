#define _GNU_SOURCE
#include "json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32

struct parser {
    const char *s;
    size_t n, i;
    struct jtok *t;
    int max, count;
};

static void ws(struct parser *p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int value(struct parser *p, int depth);

static int new_tok(struct parser *p, int type, int start)
{
    if (p->count >= p->max) return -1;
    p->t[p->count] = (struct jtok){ .type = type, .start = start, .end = start };
    return p->count++;
}

static int string(struct parser *p)
{
    int k = new_tok(p, J_STR, (int)p->i + 1);
    if (k < 0) return -1;
    p->i++;                                       // the opening quote
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            p->t[k].end = (int)p->i++;
            p->t[k].skip = p->count;
            return k;
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (++p->i >= p->n) return -1;
            char e = p->s[p->i];
            if (e == 'u') {
                if (p->i + 4 >= p->n) return -1;
                for (int j = 1; j <= 4; j++) if (hexval(p->s[p->i + j]) < 0) return -1;
                p->i += 4;
            } else if (!strchr("\"\\/bfnrt", e)) return -1;
        }
        p->i++;
    }
    return -1;
}

static int number(struct parser *p)
{
    size_t b = p->i;
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    size_t d = p->i;
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
    if (p->i == d) return -1;
    if (p->i < p->n && p->s[p->i] == '.') {
        size_t f = ++p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        if (p->i == f) return -1;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        size_t e = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++;
        if (p->i == e) return -1;
    }
    int k = new_tok(p, J_NUM, (int)b);
    if (k < 0) return -1;
    p->t[k].end = (int)p->i;
    p->t[k].skip = p->count;
    return k;
}

static int word(struct parser *p, const char *w, int type)
{
    size_t l = strlen(w);
    if (p->n - p->i < l || memcmp(p->s + p->i, w, l)) return -1;
    int k = new_tok(p, type, (int)p->i);
    if (k < 0) return -1;
    p->i += l;
    p->t[k].end = (int)p->i;
    p->t[k].skip = p->count;
    return k;
}

static int container(struct parser *p, int depth, int obj)
{
    if (depth >= MAX_DEPTH) return -1;
    int k = new_tok(p, obj ? J_OBJ : J_ARR, (int)p->i);
    if (k < 0) return -1;
    p->i++;
    ws(p);
    if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) {
        p->i++;
    } else {
        for (;;) {
            ws(p);
            if (obj) {
                if (p->i >= p->n || p->s[p->i] != '"' || string(p) < 0) return -1;
                ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') return -1;
                p->i++;
                ws(p);
            }
            if (value(p, depth + 1) < 0) return -1;
            p->t[k].size++;
            ws(p);
            if (p->i >= p->n) return -1;
            char c = p->s[p->i++];
            if (c == ',') continue;
            if (c == (obj ? '}' : ']')) break;
            return -1;
        }
    }
    p->t[k].end = (int)p->i;
    p->t[k].skip = p->count;
    return k;
}

static int value(struct parser *p, int depth)
{
    ws(p);
    if (p->i >= p->n) return -1;
    switch (p->s[p->i]) {
    case '{': return container(p, depth, 1);
    case '[': return container(p, depth, 0);
    case '"': return string(p);
    case 't': return word(p, "true", J_TRUE);
    case 'f': return word(p, "false", J_FALSE);
    case 'n': return word(p, "null", J_NULL);
    default: return number(p);
    }
}

int json_parse(const char *s, size_t n, struct jtok *t, int max)
{
    struct parser p = { .s = s, .n = n, .t = t, .max = max };
    if (value(&p, 0) < 0) return -1;
    ws(&p);
    return p.i == n ? p.count : -1;
}

int json_get(const char *s, const struct jtok *t, int obj, const char *key)
{
    if (obj < 0 || t[obj].type != J_OBJ) return -1;
    size_t kl = strlen(key);
    int i = obj + 1;
    for (int m = 0; m < t[obj].size; m++) {
        int v = i + 1;
        if ((size_t)(t[i].end - t[i].start) == kl && !memcmp(s + t[i].start, key, kl)) return v;
        i = t[v].skip;
    }
    return -1;
}

static void put_utf8(char *out, size_t *o, unsigned cp)
{
    if (cp < 0x80) out[(*o)++] = (char)cp;
    else if (cp < 0x800) { out[(*o)++] = (char)(0xC0 | cp >> 6); out[(*o)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out[(*o)++] = (char)(0xE0 | cp >> 12); out[(*o)++] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*o)++] = (char)(0xF0 | cp >> 18); out[(*o)++] = (char)(0x80 | (cp >> 12 & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp >> 6 & 0x3F)); out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static unsigned hex4(const char *h)
{
    return (unsigned)(hexval(h[0]) << 12 | hexval(h[1]) << 8 | hexval(h[2]) << 4 | hexval(h[3]));
}

int json_str(const char *s, const struct jtok *t, int i, char *out, size_t n)
{
    if (i < 0 || t[i].type != J_STR || n < 5) return -1;
    size_t o = 0;
    for (int j = t[i].start; j < t[i].end; j++) {
        if (o + 5 > n) return -1;                  // room for the longest sequence and the NUL
        char c = s[j];
        if (c != '\\') { out[o++] = c; continue; }
        c = s[++j];
        switch (c) {
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            unsigned cp = hex4(s + j + 1);
            j += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j + 6 < t[i].end && s[j + 1] == '\\' && s[j + 2] == 'u') {
                unsigned lo = hex4(s + j + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j += 6; }
            }
            if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;   // a lone surrogate
            if (cp == 0) cp = 0xFFFD;                       // no NULs inside C strings
            put_utf8(out, &o, cp);
            break;
        }
        default: out[o++] = c;                       // \" \\ \/
        }
    }
    out[o] = 0;
    return 0;
}

int json_num(const char *s, const struct jtok *t, int i, double *out)
{
    if (i < 0 || (t[i].type != J_NUM && t[i].type != J_STR)) return -1;
    int len = t[i].end - t[i].start;
    if (len <= 0 || len > 40) return -1;
    char buf[48];
    memcpy(buf, s + t[i].start, (size_t)len);
    buf[len] = 0;
    char *end;
    double v = strtod(buf, &end);
    while (*end == ' ') end++;
    if (end == buf || *end || v != v) return -1;
    *out = v;
    return 0;
}

int json_bool(const char *s, const struct jtok *t, int i, int *out)
{
    if (i < 0) return -1;
    if (t[i].type == J_TRUE || t[i].type == J_FALSE) { *out = t[i].type == J_TRUE; return 0; }
    if (t[i].type != J_STR) return -1;
    int len = t[i].end - t[i].start;
    if (len == 4 && !memcmp(s + t[i].start, "true", 4)) { *out = 1; return 0; }
    if (len == 5 && !memcmp(s + t[i].start, "false", 5)) { *out = 0; return 0; }
    return -1;
}

static int grow(struct sb *b, size_t more)
{
    if (b->oom) return -1;
    if (b->n + more + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < b->n + more + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) { b->oom = 1; return -1; }
    b->p = p;
    b->cap = cap;
    return 0;
}

void sb_raw(struct sb *b, const void *data, size_t n)
{
    if (grow(b, n)) return;
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
}

void sb_puts(struct sb *b, const char *s) { sb_raw(b, s, strlen(s)); }

void sb_printf(struct sb *b, const char *fmt, ...)
{
    char small[256];
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(small, sizeof small, fmt, a);
    va_end(a);
    if (k < 0) return;
    if ((size_t)k < sizeof small) { sb_raw(b, small, (size_t)k); return; }
    if (grow(b, (size_t)k)) return;
    va_start(a, fmt);
    vsnprintf(b->p + b->n, (size_t)k + 1, fmt, a);
    va_end(a);
    b->n += (size_t)k;
}

// The length of the valid UTF-8 sequence at c (1 to 4), or 0.
static int utf8_len(const unsigned char *c)
{
    int n = c[0] < 0x80 ? 1 : (c[0] & 0xE0) == 0xC0 ? 2 : (c[0] & 0xF0) == 0xE0 ? 3 : (c[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 2 && c[0] < 0xC2) return 0;                     // overlong
    for (int i = 1; i < n; i++) if ((c[i] & 0xC0) != 0x80) return 0;
    if (n == 3 && ((c[0] == 0xE0 && c[1] < 0xA0) || (c[0] == 0xED && c[1] >= 0xA0))) return 0;   // overlong, surrogate
    if (n == 4 && ((c[0] == 0xF0 && c[1] < 0x90) || c[0] > 0xF4 || (c[0] == 0xF4 && c[1] >= 0x90))) return 0;
    return n;
}

void sb_jstr(struct sb *b, const char *s)
{
    sb_raw(b, "\"", 1);
    for (const unsigned char *c = (const unsigned char *)s; *c;) {
        if (*c == '"' || *c == '\\') { char e[2] = { '\\', (char)*c }; sb_raw(b, e, 2); }
        else if (*c == '\n') sb_raw(b, "\\n", 2);
        else if (*c == '\r') sb_raw(b, "\\r", 2);
        else if (*c == '\t') sb_raw(b, "\\t", 2);
        else if (*c < 0x20 || *c == 0x7F) sb_printf(b, "\\u%04x", *c);
        else if (*c >= 0x80) {
            int n = utf8_len(c);                                 // a file's text may be in any encoding:
            if (n) { sb_raw(b, c, (size_t)n); c += n; continue; }
            sb_raw(b, "\xEF\xBF\xBD", 3);                       // a stray byte becomes U+FFFD
        } else sb_raw(b, c, 1);
        c++;
    }
    sb_raw(b, "\"", 1);
}

void sb_base64(struct sb *b, const uint8_t *d, size_t n)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (grow(b, (n + 2) / 3 * 4)) return;
    char *o = b->p + b->n;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        *o++ = A[v >> 18]; *o++ = A[v >> 12 & 63]; *o++ = A[v >> 6 & 63]; *o++ = A[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0);
        *o++ = A[v >> 18]; *o++ = A[v >> 12 & 63];
        *o++ = i + 1 < n ? A[v >> 6 & 63] : '=';
        *o++ = '=';
    }
    b->n = (size_t)(o - b->p);
    b->p[b->n] = 0;
}

void sb_free(struct sb *b)
{
    free(b->p);
    *b = (struct sb){ 0 };
}
