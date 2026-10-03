// The device for MCP: its status (load, memory, temperature, storage, network) and read-only access to files under
// the configured roots (mcp_files: MPC's storage by default), never outside them.
#ifndef DEVICE_H
#define DEVICE_H
#include "json.h"

// Text describing the device now (takes about 250 ms: CPU use is measured over that time).
void dev_status(struct sb *b);

// The folders files may be read from, comma-separated ("none": no file access). 0 when understood.
int files_set_roots(const char *csv);
// The roots as text, one per line, with whether each exists.
void files_roots(struct sb *b);
// A folder's entries (folders first), at most 500. 0 on success, else err says why.
int files_list(const char *path, struct sb *b, char *err, size_t en);
// Up to max bytes of a text file from offset. 0 on success.
int files_read(const char *path, long long offset, int max, struct sb *b, char *err, size_t en);
// Files and folders under path whose name matches a shell pattern (case-insensitive), at most max. 0 on success.
int files_find(const char *path, const char *pattern, int max, struct sb *b, char *err, size_t en);

#endif
