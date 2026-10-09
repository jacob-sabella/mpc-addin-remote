// Unit tests: touch mapping for each rotation, settings, query parsing, PNG framing, the JSON reader and writer,
// screenshot regions and grids, and the ALSA sequencer event layout against the real header where it is installed.
#include "../src/buttons.h"
#include "../src/conf.h"
#include "../src/image.h"
#include "../src/json.h"
#include "../src/png.h"
#include "../src/touch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#if __has_include(<alsa/seq_event.h>)
#include <alsa/asoundlib.h>
#define HAVE_ALSA_HEADER 1
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void map(int rot, int x, int y, int ex, int ey)
{
    int tx, ty;
    touch_open("auto", rot);           // REMOTE_FAKE_TOUCH: ranges 0..2047
    touch_map(x, y, 1280, 800, &tx, &ty);
    if (tx != ex || ty != ey) { printf("FAIL rot %d (%d,%d) -> (%d,%d), want (%d,%d)\n", rot, x, y, tx, ty, ex, ey); fails++; }
}

int main(void)
{
    setenv("REMOTE_FAKE_TOUCH", "/dev/null", 1);
    // rotation 90, the MPC Key 37 mapping: tx = y * 2048 / 800, ty = (1280 - x) * 2048 / 1280
    map(90, 640, 400, 400 * 2048 / 800, (1280 - 640) * 2048 / 1280);
    map(90, 100, 700, 700 * 2048 / 800, (1280 - 100) * 2048 / 1280);
    map(90, 0, 0, 0, 2047);            // (1280 - 0) would be 2048: clamped to the panel's maximum
    map(90, 5000, -3, 0, 1);           // off-screen points clamp to the edge
    map(0, 640, 400, 1024, 1024);
    map(180, 0, 0, 2047, 2047);
    map(270, 1279, 0, 2047, 2046);
    map(450, 640, 400, 1024, 1024);    // 450 wraps to 90
    {
        int tx, ty;
        touch_open("auto", 90);
        touch_map(640, 400, 1280, 800, &tx, &ty);
        CHECK(tx == 1024 && ty == 1024);
    }

    struct conf c;
    conf_defaults(&c);
    CHECK(c.port == 6720 && c.screen_rotate == -1 && c.touch_rotate == 0 && c.enabled == 1 && !strcmp(c.touch_device, "auto"));
    CHECK(conf_line(&c, "screen_rotate=180") == 0 && c.screen_rotate == 180);
    CHECK(conf_line(&c, "screen_rotate=auto") == 0 && c.screen_rotate == -1);
    CHECK(conf_line(&c, "touch_rotate=auto") && c.touch_rotate == 0);
    CHECK(conf_line(&c, "port = 8090") == 0 && c.port == 8090);
    CHECK(conf_line(&c, "  # a comment") == 0 && conf_line(&c, "") == 0);
    CHECK(conf_line(&c, "port=0") && c.port == 8090);
    CHECK(conf_line(&c, "port=80x") && c.port == 8090);
    CHECK(conf_line(&c, "touch_rotate=45") && c.touch_rotate == 0);
    CHECK(conf_line(&c, "touch_rotate=270") == 0 && c.touch_rotate == 270);
    CHECK(conf_line(&c, "touch_device=/etc/passwd") && !strcmp(c.touch_device, "auto"));
    CHECK(conf_line(&c, "touch_device=/dev/input/event3") == 0 && !strcmp(c.touch_device, "/dev/input/event3"));
    CHECK(conf_line(&c, "bind=127.0.0.1") == 0 && !strcmp(c.bind, "127.0.0.1"));
    CHECK(conf_line(&c, "bind=0000:0000:0000:0000:0000:0000:0000:0000:0000:0000:0000") && !strcmp(c.bind, "127.0.0.1"));
    CHECK(conf_line(&c, "unknown=1"));
    CHECK(conf_line(&c, "enabled=0") == 0 && c.enabled == 0);
    CHECK(conf_line(&c, "max_fps=100") && c.max_fps == 10);
    CHECK(conf_load(&c, "/nonexistent/file") == 0);

    CHECK(query_int("max=5&x=3", "x", -1, -1, 9) == 3);   // "x" isn't found inside "max"
    CHECK(query_int("x=3&y=4", "y", -1, -1, 9) == 4);
    CHECK(query_int("x=99", "x", -1, -1, 9) == 9);
    CHECK(query_int("x=-50", "x", -1, -1, 9) == -1);
    CHECK(query_int("x=", "x", 7, 0, 9) == 7);
    CHECK(query_int("x=3a", "x", 7, 0, 9) == 7);
    CHECK(query_int("xx=3", "x", 7, 0, 9) == 7);
    CHECK(query_int(NULL, "x", 7, 0, 9) == 7);
    CHECK(query_int("x=99999999999999999999", "x", 7, 0, 9) == 7);

    CHECK(png_init() == 0);
    unsigned char rgb[4 * 3 * 3];
    for (size_t i = 0; i < sizeof rgb; i++) rgb[i] = (unsigned char)(i * 7);
    size_t len = 0;
    unsigned char *p = png_encode(rgb, 4, 3, &len);
    CHECK(p && len > 57 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(p + 12, "IHDR", 4) && !memcmp(p + len - 8, "IEND", 4));
    free(p);
    CHECK(png_encode(rgb, 0, 3, &len) == NULL);
    CHECK(png_crc(rgb, sizeof rgb) != png_crc(rgb, sizeof rgb - 1));

    // MCP settings
    conf_defaults(&c);
    CHECK(c.mcp == 1 && !strcmp(c.mcp_files, "/media,/sdcard,/data/mpc-addins"));
    CHECK(conf_line(&c, "mcp=0") == 0 && c.mcp == 0);
    CHECK(conf_line(&c, "mcp_files=none") == 0 && !strcmp(c.mcp_files, "none"));
    CHECK(conf_line(&c, "mcp_files=relative/path") && !strcmp(c.mcp_files, "none"));

    // JSON
    {
        struct jtok t[64];
        const char *s = "{\"a\": [1, -2.5e3, \"x\\u00e9\\ud83c\\udfb9\\n\"], \"b\": {\"c\": true}, \"d\": null, \"e\": \"42\"}";
        int n = json_parse(s, strlen(s), t, 64);
        CHECK(n == 14 && t[0].type == J_OBJ && t[0].size == 4);
        int a = json_get(s, t, 0, "a"), b = json_get(s, t, 0, "b"), e = json_get(s, t, 0, "e");
        CHECK(a == 2 && t[a].type == J_ARR && t[a].size == 3 && json_get(s, t, 0, "zz") == -1);
        double v;
        CHECK(json_num(s, t, a + 2, &v) == 0 && v == -2500);
        CHECK(json_num(s, t, e, &v) == 0 && v == 42);      // a number in a string is taken
        char out[32];
        CHECK(json_str(s, t, a + 3, out, sizeof out) == 0 && !strcmp(out, "x\xc3\xa9\xf0\x9f\x8e\xb9\n"));
        CHECK(json_str(s, t, a + 3, out, 6));                 // too small: refused, never cut
        int bl;
        CHECK(json_bool(s, t, json_get(s, t, b, "c"), &bl) == 0 && bl == 1);
        CHECK(t[json_get(s, t, 0, "d")].type == J_NULL);
        const char *bad[] = { "", "{", "{\"a\"}", "[1,]", "{\"a\":1,}", "\"\\x\"", "\"a\nb\"", "01x", "[1] 2", "tru", "\"\\u12g4\"", "-", "1." };
        for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
            int r = json_parse(bad[i], strlen(bad[i]), t, 64);
            if (r >= 0) { printf("FAIL json accepted: %s\n", bad[i]); fails++; }
        }
        char deep[200];
        memset(deep, '[', 100);
        memset(deep + 100, ']', 100);
        CHECK(json_parse(deep, 200, t, 64) < 0);              // too deep (and too many tokens)
        CHECK(json_parse("[1,2,3]", 7, t, 3) < 0);            // too many tokens
        CHECK(json_parse("\"\\u0000\"", 8, t, 4) == 1 && json_str("\"\\u0000\"", t, 0, out, sizeof out) == 0 && !strcmp(out, "\xef\xbf\xbd"));

        struct sb o = { 0 };
        sb_jstr(&o, "q\"\\\x01\t\xc3\xa9\xff\xe0\x80\x80z");
        CHECK(o.p && !strcmp(o.p, "\"q\\\"\\\\\\u0001\\t\xc3\xa9\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbdz\""));
        sb_free(&o);
        sb_base64(&o, (const uint8_t *)"Ma", 2);
        sb_base64(&o, (const uint8_t *)"Man", 3);
        sb_base64(&o, (const uint8_t *)"M", 1);
        CHECK(o.p && !strcmp(o.p, "TWE=TWFuTQ=="));
        sb_free(&o);
        for (int i = 0; i < 3000; i++) sb_printf(&o, "%d,", i);
        CHECK(!o.oom && o.n > 10000 && !strncmp(o.p + o.n - 5, "2999,", 5));
        sb_free(&o);
    }

    // screenshot regions and grids
    {
        uint8_t rgb[8 * 6 * 3];
        for (int i = 0; i < 8 * 6; i++) { rgb[i * 3] = (uint8_t)i; rgb[i * 3 + 1] = 0; rgb[i * 3 + 2] = 0; }
        int ow, oh;
        uint8_t *r = img_region(rgb, 8, 2, 1, 4, 3, 4, &ow, &oh);          // zoom 2
        CHECK(r && ow == 8 && oh == 6 && r[0] == 1 * 8 + 2 && r[3] == 1 * 8 + 2 && r[6] == 1 * 8 + 3 && r[(5 * 8 + 7) * 3] == 3 * 8 + 5);
        free(r);
        r = img_region(rgb, 8, 0, 0, 8, 6, 1, &ow, &oh);                    // half
        CHECK(r && ow == 4 && oh == 3 && r[3] == 2 && r[(1 * 4) * 3] == 2 * 8);
        free(r);
        r = img_region(rgb, 8, 7, 5, 1, 1, 1, &ow, &oh);                    // never empty
        CHECK(r && ow == 1 && oh == 1 && r[0] == 47);
        free(r);
        CHECK(img_grid_step(2) == 100 && img_grid_step(1) == 200 && img_grid_step(8) == 20 && img_grid_step(4) == 50);
        size_t big = 300 * 200 * 3;
        uint8_t *g = calloc(1, big);
        img_grid(g, 300, 200, 50, 0, 2, 100);                               // lines at x 100, 200, 300 -> columns 50, 150, 250
        CHECK(g && g[(150 * 300 + 50) * 3 + 1] > 0 && g[(150 * 300 + 51) * 3 + 1] == 0 && g[(150 * 300 + 150) * 3 + 1] > 0);
        img_grid(g, 3, 3, 0, 0, 2, 1);                                      // labels that don't fit are clipped
        free(g);
    }

    {   // button names and profile lines
        char n[24];
        struct button b;
        CHECK(!buttons_norm("play", n, sizeof n) && !strcmp(n, "PLAY"));
        CHECK(!buttons_norm("Scene 1_a", n, sizeof n) && !strcmp(n, "SCENE-1-A"));
        CHECK(buttons_norm("", n, sizeof n) && buttons_norm("a/b", n, sizeof n) && buttons_norm("12345678901234567890123456", n, sizeof n));
        CHECK(!buttons_parse_line("PLAY=82", &b) && !strcmp(b.name, "PLAY") && b.note == 82 && b.channel == 1);
        CHECK(!buttons_parse_line("  play = 5 3  # a comment", &b) && !strcmp(b.name, "PLAY") && b.note == 5 && b.channel == 3);
        CHECK(!buttons_parse_line("MENU=0", &b) && b.note == 0 && !buttons_parse_line("X=127 16", &b) && b.channel == 16);
        CHECK(buttons_parse_line("X=128", &b) && buttons_parse_line("X=-1", &b) && buttons_parse_line("X=1 17", &b)
              && buttons_parse_line("X=1 2 3", &b) && buttons_parse_line("novalue", &b) && buttons_parse_line("a!=1", &b)
              && buttons_parse_line("=1", &b) && buttons_parse_line("# PLAY=1", &b) && buttons_parse_line("", &b));
    }

#ifdef HAVE_ALSA_HEADER
    // midi.c writes snd_seq_event_t by hand: the layout it assumes
    CHECK(sizeof(snd_seq_event_t) == 28 && offsetof(snd_seq_event_t, source) == 12 && offsetof(snd_seq_event_t, dest) == 14
          && offsetof(snd_seq_event_t, data) == 16 && offsetof(snd_seq_ev_ctrl_t, param) == 4 && offsetof(snd_seq_ev_ctrl_t, value) == 8
          && SND_SEQ_EVENT_SYSEX == 130 && SND_SEQ_EVENT_PITCHBEND == 13 && SND_SEQ_EVENT_START == 30 && SND_SEQ_EVENT_CLOCK == 36
          && SND_SEQ_QUEUE_DIRECT == 253 && SND_SEQ_ADDRESS_SUBSCRIBERS == 254 && SND_SEQ_EVENT_LENGTH_VARIABLE == 4
          && SND_SEQ_PORT_CAP_SUBS_READ == 32 && SND_SEQ_PORT_CAP_SUBS_WRITE == 64 && SND_SEQ_PORT_TYPE_APPLICATION == (1 << 20));
#else
    printf("skip ALSA layout check: no alsa/asoundlib.h here\n");
#endif

    printf(fails ? "unit: %d FAILED\n" : "unit: all passed\n", fails);
    return fails != 0;
}
