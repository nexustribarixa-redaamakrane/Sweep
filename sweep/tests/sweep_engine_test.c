/* sweep_engine_test.c - standalone host test for the Sweep engine slice.
 * Build: gcc -std=c99 -Wall -Wextra -Werror sweep/sweep_engine.c sweep/tests/sweep_engine_test.c -o sweep_engine_test && ./sweep_engine_test
 */
#include <stdio.h>
#include <string.h>

#include "../sweep_engine.h"

static int g_failures = 0;
static int g_passed = 0;

static void check(const char* label, bool got, bool want) {
    if (got == want) {
        ++g_passed;
        printf("[PASS] %s = %s\n", label, want ? "true" : "false");
    } else {
        ++g_failures;
        printf("[FAIL] %s: got %s, want %s\n", label, got ? "true" : "false",
               want ? "true" : "false");
    }
}

int main(void) {
    SWEEP_URL url;
    SWEEP_VIEWPORT requested;
    SWEEP_VIEWPORT negotiated;
    SWEEP_PAINT_CMD list[4];
    size_t count = 0u;
    SWEEP_PAINT_CMD cmd;

    check("http URL parses", SweepUrlParse("http://example.com/a/b", &url), true);
    check("host extracted", strcmp(url.host, "example.com") == 0, true);
    check("path extracted", strcmp(url.path, "/a/b") == 0, true);
    check("no port by default", url.has_port, false);
    check("https URL with port parses",
          SweepUrlParse("https://sub.example.org:8443/", &url), true);
    check("port parsed", url.has_port && url.port == 8443u, true);
    check("ftp URL refused", SweepUrlParse("ftp://example.com/", &url), false);
    check("relative URL refused", SweepUrlParse("/just/a/path", &url), false);
    check("empty host refused", SweepUrlParse("http:///path", &url), false);
    check("null text refused", SweepUrlParse((const char*)0, &url), false);
    check("bad port refused", SweepUrlParse("http://example.com:99999/", &url),
          false);

    requested.width_px = 1920u;
    requested.height_px = 1080u;
    requested.device_scale = 2u;
    check("viewport negotiates", SweepViewportNegotiate(&requested, &negotiated),
          true);
    check("viewport width kept", negotiated.width_px == 1920u, true);
    check("viewport scale kept", negotiated.device_scale == 2u, true);

    requested.width_px = 64000u;
    requested.height_px = 20000u;
    requested.device_scale = 16u;
    check("oversized viewport negotiates", SweepViewportNegotiate(&requested, &negotiated), true);
    check("oversized width clamped", negotiated.width_px == 8192u, true);
    check("oversized scale clamped", negotiated.device_scale == 4u, true);

    requested.width_px = 0u;
    check("empty viewport refused", SweepViewportNegotiate(&requested, &negotiated), false);

    memset(&cmd, 0, sizeof(cmd));
    cmd.op = (uint8_t)SWEEP_PAINT_CLEAR;
    check("clear command appended", SweepPaintAppend(list, 4u, &count, cmd), true);
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = (uint8_t)SWEEP_PAINT_RECT;
    cmd.x = 10; cmd.y = 20; cmd.w = 100u; cmd.h = 50u; cmd.color = 0xFF0000u;
    check("rect command appended", SweepPaintAppend(list, 4u, &count, cmd), true);
    check("two commands counted", count == 2u, true);
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = (uint8_t)SWEEP_PAINT_TEXT;
    check("text without glyph refused", SweepPaintAppend(list, 4u, &count, cmd), false);
    cmd.glyph = (uint32_t)'A';
    check("text with glyph appended", SweepPaintAppend(list, 4u, &count, cmd), true);
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = 99u;
    check("unknown op refused", SweepPaintAppend(list, 4u, &count, cmd), false);

    printf("=== Sweep engine: %d passed, %d failed ===\n", g_passed, g_failures);
    return g_failures != 0;
}
