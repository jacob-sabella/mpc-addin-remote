// A small JSON reader (a flat token array over the source text, no allocation) and a growable output buffer with
// JSON string escaping and base64, for the MCP endpoint.
#ifndef JSON_H
#define JSON_H
#include <stddef.h>
#include <stdint.h>

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

struct jtok {
    int type;
    int start, end;   // the token's bytes in the source (a string's without its quotes)
    int size;         // children: array items, or object members (each member is a key token then a value)
    int skip;         // the index just past this token's subtree
};

// Parse n bytes of s into at most max tokens. Returns the token count, or -1 (bad JSON, too deep, too many tokens,
// or anything but white space after the value). Token 0 is the top value.
int json_parse(const char *s, size_t n, struct jtok *t, int max);
// The value token of key in object token obj, or -1. Keys are compared as written (no escapes in the key looked for).
int json_get(const char *s, const struct jtok *t, int obj, const char *key);
// String token i, unescaped into out (UTF-8, NUL-terminated). 0 on success, -1 if not a string or too long.
int json_str(const char *s, const struct jtok *t, int i, char *out, size_t n);
// Number token i (or a string holding just a number: models send "120" too). 0 on success.
int json_num(const char *s, const struct jtok *t, int i, double *out);
// true/false token i (or the strings "true"/"false"). 0 on success.
int json_bool(const char *s, const struct jtok *t, int i, int *out);

// Output buffer. On allocation failure it stops growing and sets oom; the caller checks once at the end.
struct sb {
    char *p;
    size_t n, cap;
    int oom;
};
void sb_raw(struct sb *b, const void *data, size_t n);
void sb_puts(struct sb *b, const char *s);
void sb_printf(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_jstr(struct sb *b, const char *s);   // s as a quoted, escaped JSON string
void sb_base64(struct sb *b, const uint8_t *data, size_t n);
void sb_free(struct sb *b);

#endif
