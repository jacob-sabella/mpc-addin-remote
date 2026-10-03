// Unit tests: touch mapping for each rotation, settings, query parsing, PNG framing.
#include "../src/conf.h"
#include "../src/png.h"
#include "../src/touch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    // rotation 90 is the Key 37's verified mapping: tx = y * 2048 / 800, ty = (1280 - x) * 2048 / 1280
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
    CHECK(c.port == 8080 && c.touch_rotate == 90 && c.enabled == 1 && !strcmp(c.touch_device, "auto"));
    CHECK(conf_line(&c, "port = 8090") == 0 && c.port == 8090);
    CHECK(conf_line(&c, "  # a comment") == 0 && conf_line(&c, "") == 0);
    CHECK(conf_line(&c, "port=0") && c.port == 8090);
    CHECK(conf_line(&c, "port=80x") && c.port == 8090);
    CHECK(conf_line(&c, "touch_rotate=45") && c.touch_rotate == 90);
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

    printf(fails ? "unit: %d FAILED\n" : "unit: all passed\n", fails);
    return fails != 0;
}
