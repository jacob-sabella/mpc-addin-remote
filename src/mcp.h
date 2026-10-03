// MCP (Model Context Protocol) over the addin's HTTP port: POST /mcp, JSON-RPC 2.0, one JSON reply per request
// (the "Streamable HTTP" transport without server-sent events). Tools let a model see the screen and touch it.
#ifndef MCP_H
#define MCP_H
#include <stddef.h>

// One POSTed message. Returns the HTTP status; *out gets the JSON reply (malloc'd, *outlen bytes), or NULL for 202
// (a notification or a response needs no reply).
int mcp_handle(const char *body, size_t n, char **out, size_t *outlen);

// Gestures from MCP and HTTP /tap don't interleave: each holds this while its finger is down.
void gesture_lock(void);
void gesture_unlock(void);

#endif
