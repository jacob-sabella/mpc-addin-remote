// Settings: key=value lines in mpc_remote_addin.conf next to the .so (or $MPC_REMOTE_ADDIN_CONF).
#ifndef CONF_H
#define CONF_H

struct conf {
    int enabled;              // enabled=0 keeps the addin loaded but idle
    char bind[46];            // listen address (0.0.0.0: every interface)
    int port;
    char touch_device[64];    // auto, or /dev/input/eventN
    int screen_rotate;        // -1 auto, or 0, 90, 180, 270: the clockwise turn that makes the scanout upright
    int touch_rotate;         // 0, 90, 180 or 270: how the touch panel sits against the scanout
    int max_fps;              // stream frame cap
    int max_clients;          // connections at once (streams count)
    int nice;                 // the addin threads' nice value, 0..19
    int mcp;                  // mcp=0 turns the MCP endpoint (/mcp) off
    char mcp_files[256];      // folders MCP may read files from, comma-separated, or none
};

void conf_defaults(struct conf *c);
// Apply one line; 0 when it was understood (or blank or a comment), -1 otherwise.
int conf_line(struct conf *c, const char *line);
// Read a file over the defaults already in c; a missing file is not an error. Returns the bad lines' count.
int conf_load(struct conf *c, const char *path);

// The query value of key as an int clamped to lo..hi, def when absent or not a number.
int query_int(const char *q, const char *key, int def, int lo, int hi);

#endif
