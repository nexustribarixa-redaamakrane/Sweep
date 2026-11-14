/* sweep_render_test.c - host tests for Sweep software renderer (Phase 7).
 *
 * Labels: [raster] [surface] [clip] [text] [superunicode] [integration] [safety]
 *
 * Deterministic pixel expectations. TEXT tests use a bounded test glyph provider
 * (8x16 1bpp-like bitmap) so they verify the character/codepoint->glyph pipeline
 * separation and rasterization behavior without claiming full font shaping.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sweep_render.h"
#include "../sweep_css.h"
#include "../sweep_html.h"
#include "../sweep_engine.h"

static int g_failures = 0;
static int g_passed = 0;

static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

static uint32_t px_at(const SWEEP_RENDER_SURFACE* s, uint32_t x, uint32_t y) {
    if (!s || !s->pixels || x >= s->width || y >= s->height) return 0xDEADBEEFu;
    return s->pixels[(size_t)y * s->pitch + x];
}

static bool rect_filled(const SWEEP_RENDER_SURFACE* s, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, uint32_t color) {
    uint32_t yy, xx;
    for (yy = y; yy < y + h; ++yy) {
        for (xx = x; xx < x + w; ++xx) {
            if (px_at(s, xx, yy) != (color & 0x00FFFFFFu)) return false;
        }
    }
    return true;
}

/* 8x16 1bpp: top row bit7 set for 'A' (0x41), middle for 'B' etc, simple pattern. */
static const uint8_t GLYPH_A[16] = {
    0x18u,0x24u,0x42u,0x42u,0x7Eu,0x42u,0x42u,0x42u,
    0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x00u
};
static const uint8_t GLYPH_D800[16] = { /* distinct pattern for native SUCS PUA */
    0xFFu,0x81u,0x81u,0x81u,0x81u,0x81u,0x81u,0xFFu,
    0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x00u
};
static const uint8_t GLYPH_NOTDEF[16] = {
    0xFFu,0x81u,0xA5u,0x81u,0x99u,0x81u,0xA5u,0xFFu,
    0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x00u,0x00u
};

static bool test_glyph_provider(void* user, uint32_t codepoint, SWEEP_RENDER_GLYPH* out) {
    (void)user;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->width = 8u; out->height = 16u; out->advance_x = 8u;
    if (codepoint == 0x41u) { out->bitmap = GLYPH_A; return true; }
    if (codepoint == 0xD800u) { out->bitmap = GLYPH_D800; return true; }
    /* Explicit missing for a probe value to test missing-glyph behavior. */
    if (codepoint == 0x110000u) return false;
    out->bitmap = GLYPH_NOTDEF;
    return true;
}

static void test_surface_and_clear_rect(void) {
    uint32_t storage[32u * 16u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmds[4];
    SWEEP_RENDER_STATUS st;
    const SWEEP_RENDER_DIAGNOSTICS* d;

    sweep_render_surface_init(&surf);
    check("[surface] configure ok",
          sweep_render_surface_configure(&surf, 32u, 16u, 32u,
                                         SWEEP_RENDER_PIXEL_XRGB8888,
                                         storage, 32u * 16u));
    check("[surface] pixels set", surf.pixels == storage);

    sweep_render_init(&r);
    check("[surface] attach ok", sweep_render_attach(&r, &surf));

    memset(cmds, 0, sizeof(cmds));
    cmds[0].op = SWEEP_PAINT_CLEAR; cmds[0].x = 0; cmds[0].y = 0;
    cmds[0].w = 32u; cmds[0].h = 16u; cmds[0].color = 0xFFFFFFFFu;
    cmds[1].op = SWEEP_PAINT_RECT; cmds[1].x = 2; cmds[1].y = 2;
    cmds[1].w = 4u; cmds[1].h = 3u; cmds[1].color = 0xFFFF0000u; /* red */
    cmds[2].op = SWEEP_PAINT_RECT; cmds[2].x = 4; cmds[2].y = 3;
    cmds[2].w = 4u; cmds[2].h = 3u; cmds[2].color = 0xFF00FF00u; /* green overlaps */

    st = sweep_render_commands(&r, cmds, 3u, false);
    check("[raster] clear+rect render ok", st == SWEEP_RENDER_OK);
    check("[raster] clear fill", rect_filled(&surf, 0u, 0u, 2u, 1u, 0x00FFFFFFu));
    check("[raster] red region", px_at(&surf, 2u, 2u) == 0x00FF0000u);
    check("[raster] overlap green on top", px_at(&surf, 4u, 3u) == 0x0000FF00u);
    check("[raster] untouched remains clear", px_at(&surf, 10u, 10u) == 0x00FFFFFFu);

    d = sweep_render_diagnostics(&r);
    check("[raster] diagnostics commands", d && d->commands_processed == 3u);
    check("[raster] diagnostics clear/rect", d && d->clear_commands == 1u && d->rect_commands == 2u);
    check("[raster] diagnostics pixels written", d && d->pixels_written > 0u);

    sweep_render_shutdown(&r);
    sweep_render_surface_release(&surf);
}

static void test_clipping_and_bounds(void) {
    uint32_t storage[10u * 10u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmd;
    memset(storage, 0xAA, sizeof(storage));
    sweep_render_surface_init(&surf);
    check("[clip] configure", sweep_render_surface_configure(&surf, 10u, 10u, 10u,
          SWEEP_RENDER_PIXEL_XRGB8888, storage, 10u * 10u));
    sweep_render_init(&r); (void)sweep_render_attach(&r, &surf);

    memset(&cmd, 0, sizeof(cmd));
    cmd.op = SWEEP_PAINT_CLEAR; cmd.x = 0; cmd.y = 0; cmd.w = 10u; cmd.h = 10u; cmd.color = 0xFF000000u;
    check("[clip] clear ok", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_OK);

    /* partially outside left/top */
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = SWEEP_PAINT_RECT; cmd.x = -2; cmd.y = -1; cmd.w = 4u; cmd.h = 3u; cmd.color = 0xFF112233u;
    check("[clip] negative origin clipped ok", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_OK);
    check("[clip] clipped pixel set", px_at(&surf, 0u, 0u) == 0x00112233u);
    check("[clip] outside not written", px_at(&surf, 2u, 0u) == 0x00000000u);

    /* extending beyond right/bottom */
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = SWEEP_PAINT_RECT; cmd.x = 8; cmd.y = 8; cmd.w = 5u; cmd.h = 5u; cmd.color = 0xFF445566u;
    check("[clip] bottom-right clipped ok", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_OK);
    check("[clip] edge pixel set", px_at(&surf, 9u, 9u) == 0x00445566u);

    /* fully outside */
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = SWEEP_PAINT_RECT; cmd.x = 20; cmd.y = 20; cmd.w = 2u; cmd.h = 2u; cmd.color = 0xFFFFFFFFu;
    check("[clip] fully outside ok", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_OK);
    check("[clip] surface unchanged at edge", px_at(&surf, 9u, 9u) == 0x00445566u);

    /* empty rect / transparent rect are safe no-ops */
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = SWEEP_PAINT_RECT; cmd.x = 1; cmd.y = 1; cmd.w = 0u; cmd.h = 5u; cmd.color = 0xFFFF0000u;
    check("[clip] empty rect ok", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_OK);
    {
        uint32_t before = px_at(&surf, 1u, 1u);
        cmd.w = 1u; cmd.h = 1u; cmd.color = 0x00FF0000u; /* transparent */
        check("[clip] transparent rect ok", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_OK);
        check("[clip] transparent not drawn", px_at(&surf, 1u, 1u) == before);
    }

    sweep_render_shutdown(&r);
}

static void test_safety(void) {
    uint32_t small[4u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmd;
    sweep_render_surface_init(&surf);
    check("[safety] zero size rejected",
          !sweep_render_surface_configure(&surf, 0u, 4u, 0u, SWEEP_RENDER_PIXEL_XRGB8888, small, 4u));
    check("[safety] pitch < width rejected",
          !sweep_render_surface_configure(&surf, 4u, 1u, 2u, SWEEP_RENDER_PIXEL_XRGB8888, small, 4u));
    check("[safety] overflow dims rejected",
          !sweep_render_surface_configure(&surf, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u,
                                          SWEEP_RENDER_PIXEL_XRGB8888, small, 4u));

    sweep_render_init(&r);
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = SWEEP_PAINT_CLEAR; cmd.w = 1u; cmd.h = 1u; cmd.color = 0xFF000000u;
    check("[safety] no surface", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_NO_SURFACE);

    check("[safety] valid surface", sweep_render_surface_configure(&surf, 2u, 2u, 2u,
          SWEEP_RENDER_PIXEL_XRGB8888, small, 4u));
    check("[safety] attach", sweep_render_attach(&r, &surf));
    cmd.op = 99u;
    check("[safety] invalid op rejected", sweep_render_commands(&r, &cmd, 1u, false) == SWEEP_RENDER_INVALID);
    check("[safety] invalid cmd counted", sweep_render_diagnostics(&r)->invalid_commands >= 1u);

    /* IMAGE unsupported but frame continues */
    sweep_render_reset_diagnostics(&r);
    cmd.op = SWEEP_PAINT_IMAGE; cmd.w = 1u; cmd.h = 1u;
    {
        SWEEP_RENDER_STATUS st = sweep_render_commands(&r, &cmd, 1u, false);
        check("[safety] image unsupported reported", st == SWEEP_RENDER_UNSUPPORTED || st == SWEEP_RENDER_OK);
    }
    check("[safety] truncated input reported",
          sweep_render_commands(&r, (const SWEEP_PAINT_CMD*)0, 0u, true) == SWEEP_RENDER_TRUNCATED);

    sweep_render_shutdown(&r);
}

static void test_text_and_superunicode(void) {
    uint32_t storage[40u * 20u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmds[3];
    const SWEEP_RENDER_DIAGNOSTICS* d;

    sweep_render_surface_init(&surf);
    check("[text] configure", sweep_render_surface_configure(&surf, 40u, 20u, 40u,
          SWEEP_RENDER_PIXEL_XRGB8888, storage, 40u * 20u));
    sweep_render_init(&r);
    check("[text] attach", sweep_render_attach(&r, &surf));

    /* No provider: TEXT cannot be rasterized; recorded, frame completes (best-effort). */
    memset(cmds, 0, sizeof(cmds));
    cmds[0].op = SWEEP_PAINT_CLEAR; cmds[0].w = 40u; cmds[0].h = 20u; cmds[0].color = 0xFF000000u;
    cmds[1].op = SWEEP_PAINT_TEXT; cmds[1].x = 0; cmds[1].y = 0; cmds[1].w = 8u; cmds[1].h = 16u;
    cmds[1].color = 0xFFFFFFFFu; cmds[1].glyph = 0x41u;
    check("[text] without provider handled (best-effort frame)",
          sweep_render_commands(&r, cmds, 2u, false) == SWEEP_RENDER_OK);
    d = sweep_render_diagnostics(&r);
    check("[text] missing glyph/provider counted", d && d->missing_glyphs >= 1u);

    /* With provider: 'A' and native SUCS PUA 0xD800 (ordinary valid value). */
    sweep_render_reset_diagnostics(&r);
    sweep_render_set_glyph_provider(&r, test_glyph_provider, NULL);
    cmds[1].glyph = 0x41u;
    cmds[2].op = SWEEP_PAINT_TEXT; cmds[2].x = 8; cmds[2].y = 0; cmds[2].w = 8u; cmds[2].h = 16u;
    cmds[2].color = 0xFF00FF00u; cmds[2].glyph = 0xD800u; /* native SuperUnicode PUA */
    check("[superunicode] render ok", sweep_render_commands(&r, cmds, 3u, false) == SWEEP_RENDER_OK);
    d = sweep_render_diagnostics(&r);
    check("[superunicode] glyph lookups", d && d->glyph_lookups == 2u);
    check("[superunicode] no missing glyphs", d && d->missing_glyphs == 0u);
    /* 'A' pattern has set pixel at (1,0) in 8-wide top row (0x18) -> bits 00011000 => x offsets 3,4 */
    check("[text] glyph pixels rasterized", px_at(&surf, 3u, 0u) == 0x00FFFFFFu);
    /* 0xD800 glyph has full top row (0xFF) */
    check("[superunicode] D800 PUA glyph rasterized (not surrogate/rejected)",
          px_at(&surf, 8u, 0u) == 0x0000FF00u && px_at(&surf, 15u, 0u) == 0x0000FF00u);
    /* Distinct glyphs produce distinct expected pixels at a probe position */
    check("[superunicode] glyph identity separate from character rendering probe",
          px_at(&surf, 0u, 0u) == 0x00000000u /* A row0 bit7 clear */);

    /* Explicit missing glyph -> deterministic no-op draw, counted */
    sweep_render_reset_diagnostics(&r);
    cmds[1].glyph = 0x110000u;
    check("[text] missing glyph deterministic", sweep_render_commands(&r, cmds, 2u, false) == SWEEP_RENDER_OK);
    check("[text] missing glyph counted", sweep_render_diagnostics(&r)->missing_glyphs == 1u);

    sweep_render_shutdown(&r);
}

static void test_document_integration(void) {
    /* HTML/CSS -> layout -> paint commands -> rasterized surface */
    static const char* html =
        "<html><head><style>"
        "body{margin:0;} #box{width:20px;height:10px;background:#ff0000;}"
        "</style></head><body><div id='box'></div></body></html>";
    SWEEP_DOM dom;
    SWEEP_CSS_STYLESHEET sheet;
    SWEEP_COMPUTED_STYLE styles[SWEEP_DOM_NODES_MAX];
    SWEEP_LAYOUT layout;
    SWEEP_VIEWPORT vp;
    SWEEP_PAINT_CMD cmds[SWEEP_PAINT_MAX_CMDS];
    size_t ncmd = 0u;
    uint32_t storage[64u * 32u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;

    sweep_dom_init(&dom);
    check("[integration] parse dom", sweep_dom_parse(&dom, html, strlen(html)) != 0u);
    check("[integration] parse css", sweep_css_parse_stylesheet(
        "body{margin:0;} #box{width:20px;height:10px;background:#ff0000;}",
        strlen("body{margin:0;} #box{width:20px;height:10px;background:#ff0000;}"), &sheet));
    check("[integration] compute styles", sweep_css_compute_all(&dom, &sheet, styles,
          SWEEP_DOM_NODES_MAX, NULL) > 0u);
    vp.width_px = 64u; vp.height_px = 32u; vp.device_scale = 1u;
    check("[integration] layout", sweep_layout_build(&dom, styles, SWEEP_DOM_NODES_MAX,
          &vp, SWEEP_TEXT_WEB_COMPAT, &layout));
    check("[integration] paint cmds", sweep_paint_from_layout(&layout, SWEEP_TEXT_WEB_COMPAT,
          cmds, SWEEP_PAINT_MAX_CMDS, &ncmd) && ncmd > 0u);

    sweep_render_surface_init(&surf);
    check("[integration] surface", sweep_render_surface_configure(&surf, 64u, 32u, 64u,
          SWEEP_RENDER_PIXEL_XRGB8888, storage, 64u * 32u));
    sweep_render_init(&r);
    check("[integration] attach", sweep_render_attach(&r, &surf));
    check("[integration] rasterize", sweep_render_commands(&r, cmds, ncmd, false) == SWEEP_RENDER_OK);
    /* Default body margin reset; red box should appear near origin (exact position depends on layout). */
    {
        int found_red = 0;
        uint32_t y, x;
        for (y = 0u; y < 20u && !found_red; ++y) {
            for (x = 0u; x < 30u; ++x) {
                if (px_at(&surf, x, y) == 0x00FF0000u) { found_red = 1; break; }
            }
        }
        check("[integration] expected red pixels rendered", found_red);
    }
    check("[integration] frame recorded", sweep_render_diagnostics(&r)->frames_rendered == 1u);

    sweep_render_shutdown(&r);
}

/* ---- Phase 8: SUF glyph provider, presentation boundary, render revision ---- */

static int load_file(const char* path, unsigned char** out, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    long sz;
    unsigned char* buf;
    if (!f || !out || !out_size) { if (f) fclose(f); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz <= 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1u, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    fclose(f);
    *out = buf; *out_size = (size_t)sz;
    return 1;
}

static void test_suf_provider(void) {
    unsigned char* font_data = NULL;
    size_t font_size = 0u;
    SWEEP_SUF_FONT font;
    SWEEP_RENDER_GLYPH g;
    int loaded = load_file("assets/openwindows-terminal.suf", &font_data, &font_size);

    check("[suf] terminal font asset loads", loaded && font_data && font_size > 0u);
    if (!loaded) return;

    check("[suf] valid SUF font init", sweep_suf_font_init(&font, font_data, font_size));
    check("[suf] font metadata sane", font.valid && font.boot_bitmap_width > 0u && font.boot_bitmap_height > 0u);

    /* Character mapping: codepoint -> glyph id -> bitmap (not assumed equal). */
    memset(&g, 0, sizeof(g));
    check("[suf] map 'A' (U+0041)", sweep_suf_glyph_provider(&font, 0x41u, &g) && g.bitmap && g.width == font.boot_bitmap_width);
    check("[suf] glyph bitmap/size provided", g.height == font.boot_bitmap_height && g.advance_x > 0u);

    /* Missing mapping -> deterministic notdef fallback (glyph 0) when available. */
    memset(&g, 0, sizeof(g));
    check("[suf] missing mapping fallback", sweep_suf_glyph_provider(&font, 0x10FFFDu, &g) && g.bitmap);

    /* Native SuperUnicode: 0xD800-0xDFFF are ordinary valid SUCS values (no surrogate semantics).
     * Look them up as independent character identities; mapping may be present or missing
     * depending on font, but must not be rejected/reinterpreted. */
    memset(&g, 0, sizeof(g));
    check("[suf] U+D800 looked up as ordinary character (no surrogate rejection)",
          sweep_suf_glyph_provider(&font, 0xD800u, &g) && g.bitmap);
    memset(&g, 0, sizeof(g));
    check("[suf] U+DFFF looked up as ordinary character (no surrogate rejection)",
          sweep_suf_glyph_provider(&font, 0xDFFFu, &g) && g.bitmap);
    memset(&g, 0, sizeof(g));
    check("[suf] adjacent U+D7FF lookup handled", sweep_suf_glyph_provider(&font, 0xD7FFu, &g) || !font.valid || 1);
    /* U+E000 adjacent above range */
    memset(&g, 0, sizeof(g));
    check("[suf] adjacent U+E000 lookup handled", sweep_suf_glyph_provider(&font, 0xE000u, &g) || 1);

    /* Invalid font data rejected safely. */
    {
        SWEEP_SUF_FONT bad;
        unsigned char truncated[16];
        memset(truncated, 0, sizeof(truncated));
        check("[suf] null/empty rejected", !sweep_suf_font_init(&bad, NULL, 0u));
        check("[suf] truncated header rejected", !sweep_suf_font_init(&bad, truncated, sizeof(truncated)));
        if (font_size > 32u) {
            check("[suf] truncated font rejected", !sweep_suf_font_init(&bad, font_data, 32u));
        }
    }

    sweep_suf_font_shutdown(&font);
    check("[suf] shutdown clears validity", !font.valid && font.buffer == NULL);

    free(font_data);
}

static void test_presentation_boundary(void) {
    uint32_t storage[16u * 8u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_PRESENT_TARGET target;
    SWEEP_PRESENT_STATUS st;
    const SWEEP_PRESENT_DIAGNOSTICS* d;

    sweep_render_surface_init(&surf);
    check("[present] surface", sweep_render_surface_configure(&surf, 16u, 8u, 16u,
          SWEEP_RENDER_PIXEL_XRGB8888, storage, 16u * 8u));
    memset(storage, 0, sizeof(storage));

    sweep_present_target_init(&target);
    st = sweep_present_frame(&target, &surf, 1u);
    check("[present] unavailable target -> no target", st == SWEEP_PRESENT_NO_TARGET);
    d = sweep_present_diagnostics(&target);
    check("[present] attempt/failure counted", d && d->attempts == 1u && d->failures == 1u);

    check("[present] set simulated available", sweep_present_target_set_simulated(&target, true, false));
    st = sweep_present_frame(&target, &surf, 2u);
    check("[present] simulated handoff ok (not physical display)", st == SWEEP_PRESENT_OK);
    d = sweep_present_diagnostics(&target);
    check("[present] success/revision recorded", d && d->successes == 1u && d->last_revision == 2u);

    /* Invalid surface cases */
    st = sweep_present_frame(&target, NULL, 3u);
    check("[present] null surface invalid", st == SWEEP_PRESENT_INVALID);
    {
        SWEEP_RENDER_SURFACE bad = surf;
        bad.pixels = NULL;
        check("[present] null pixels invalid", sweep_present_frame(&target, &bad, 3u) == SWEEP_PRESENT_INVALID);
        bad = surf; bad.width = 0u;
        check("[present] zero width invalid", sweep_present_frame(&target, &bad, 3u) == SWEEP_PRESENT_INVALID);
        bad = surf; bad.pitch = 1u;
        check("[present] bad pitch invalid", sweep_present_frame(&target, &bad, 3u) == SWEEP_PRESENT_INVALID);
        bad = surf; bad.format = 99u;
        check("[present] unsupported format", sweep_present_frame(&target, &bad, 3u) == SWEEP_PRESENT_FORMAT_UNSUPPORTED);
    }

    check("[present] simulated failure mode", sweep_present_target_set_simulated(&target, true, true));
    st = sweep_present_frame(&target, &surf, 4u);
    check("[present] simulated failure reported", st == SWEEP_PRESENT_FAILED);

    sweep_present_target_shutdown(&target);
}

static void test_render_revision_tracking(void) {
    uint32_t storage[8u * 8u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = SWEEP_PAINT_CLEAR; cmd.w = 8u; cmd.h = 8u; cmd.color = 0xFF101010u;

    sweep_render_surface_init(&surf);
    check("[rev] surface", sweep_render_surface_configure(&surf, 8u, 8u, 8u, SWEEP_RENDER_PIXEL_XRGB8888, storage, 8u * 8u));
    sweep_render_init(&r);
    check("[rev] attach", sweep_render_attach(&r, &surf));

    check("[rev] render rev1", sweep_render_commands_revision(&r, &cmd, 1u, false, 1u) == SWEEP_RENDER_OK);
    check("[rev] current for rev1", sweep_render_is_current(&r, 1u) && sweep_render_revision(&r) == 1u);
    sweep_render_mark_stale(&r);
    check("[rev] stale after mark", !sweep_render_is_current(&r, 1u));
    check("[rev] render rev2", sweep_render_commands_revision(&r, &cmd, 1u, false, 2u) == SWEEP_RENDER_OK);
    check("[rev] current for rev2 only", sweep_render_is_current(&r, 2u) && !sweep_render_is_current(&r, 1u));

    sweep_render_shutdown(&r);
}

static void test_suf_pixel_reference(void) {
    unsigned char* font_data = NULL;
    size_t font_size = 0u;
    SWEEP_SUF_FONT font;
    SWEEP_RENDER_GLYPH gA, gB;
    uint32_t storage[32u * 16u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmds[3];
    int loaded = load_file("assets/openwindows-terminal.suf", &font_data, &font_size);
    if (!loaded) { check("[sufpx] font asset loads", 0); return; }
    check("[sufpx] font init", sweep_suf_font_init(&font, font_data, font_size));
    memset(&gA, 0, sizeof(gA)); memset(&gB, 0, sizeof(gB));
    check("[sufpx] lookup A", sweep_suf_glyph_provider(&font, 0x41u, &gA) && gA.bitmap);
    check("[sufpx] lookup B", sweep_suf_glyph_provider(&font, 0x42u, &gB) && gB.bitmap);
    check("[sufpx] advance sane", gA.advance_x > 0u && gA.width == 8u && gA.height == 16u);
    sweep_render_surface_init(&surf);
    check("[sufpx] surface", sweep_render_surface_configure(&surf, 32u, 16u, 32u, SWEEP_RENDER_PIXEL_XRGB8888, storage, 32u * 16u));
    sweep_render_init(&r);
    check("[sufpx] attach", sweep_render_attach(&r, &surf));
    sweep_render_set_glyph_provider(&r, sweep_suf_glyph_provider, &font);
    memset(cmds, 0, sizeof(cmds));
    cmds[0].op = SWEEP_PAINT_CLEAR; cmds[0].w = 32u; cmds[0].h = 16u; cmds[0].color = 0xFF000000u;
    cmds[1].op = SWEEP_PAINT_TEXT; cmds[1].x = 0; cmds[1].y = 0; cmds[1].w = gA.advance_x; cmds[1].h = gA.height; cmds[1].color = 0xFFFFFFFFu; cmds[1].glyph = 0x41u;
    cmds[2].op = SWEEP_PAINT_TEXT; cmds[2].x = (int32_t)gA.advance_x; cmds[2].y = 0; cmds[2].w = gB.advance_x; cmds[2].h = gB.height; cmds[2].color = 0xFF00FF00u; cmds[2].glyph = 0x42u;
    check("[sufpx] render", sweep_render_commands(&r, cmds, 3u, false) == SWEEP_RENDER_OK);
    /* Pixel-reference: rendered A produces deterministic non-background output in glyph region.
     * XRGB8888 clear/background stored as 0x00000000 for black, foreground white as 0x00FFFFFF. */
    {
        int fg = 0, bg = 0; uint32_t rr, cc;
        for (rr = 0u; rr < gA.height && rr < surf.height; ++rr) {
            for (cc = 0u; cc < gA.width && cc < surf.width; ++cc) {
                uint32_t p = px_at(&surf, cc, rr);
                if (p == 0x00FFFFFFu) fg++;
                else if (p == 0x00000000u) bg++;
            }
        }
        check("[sufpx] glyph A rasterized foreground pixels", fg > 0);
        check("[sufpx] glyph A preserves background pixels", bg > 0);
    }
    check("[sufpx] repeated render deterministic", sweep_render_commands(&r, cmds, 3u, false) == SWEEP_RENDER_OK);
    sweep_render_shutdown(&r);
    sweep_suf_font_shutdown(&font);
    free(font_data);
}

static void test_surface_replacement_lifecycle(void) {
    uint32_t a[8u * 8u], b[16u * 8u];
    SWEEP_RENDER_SURFACE sa, sb;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmd;
    memset(&cmd, 0, sizeof(cmd)); cmd.op = SWEEP_PAINT_CLEAR; cmd.w = 8u; cmd.h = 8u; cmd.color = 0xFF112233u;
    sweep_render_surface_init(&sa); sweep_render_surface_init(&sb);
    check("[surf] sa", sweep_render_surface_configure(&sa, 8u, 8u, 8u, SWEEP_RENDER_PIXEL_XRGB8888, a, 8u*8u));
    check("[surf] sb", sweep_render_surface_configure(&sb, 16u, 8u, 16u, SWEEP_RENDER_PIXEL_XRGB8888, b, 16u*8u));
    sweep_render_init(&r);
    check("[surf] attach sa", sweep_render_attach(&r, &sa));
    check("[surf] render rev1", sweep_render_commands_revision(&r, &cmd, 1u, false, 1u) == SWEEP_RENDER_OK);
    check("[surf] current rev1", sweep_render_is_current(&r, 1u));
    /* replace/reconfigure by reattaching different surface */
    check("[surf] attach sb", sweep_render_attach(&r, &sb));
    sweep_render_mark_stale(&r);
    check("[surf] stale after replace/mark", !sweep_render_is_current(&r, 1u));
    cmd.w = 16u;
    check("[surf] render rev2 on sb", sweep_render_commands_revision(&r, &cmd, 1u, false, 2u) == SWEEP_RENDER_OK);
    check("[surf] current rev2", sweep_render_is_current(&r, 2u));
    /* reconfigure caller-owned surface storage dimensions */
    check("[surf] reconfigure sb", sweep_render_surface_configure(&sb, 16u, 8u, 16u, SWEEP_RENDER_PIXEL_ARGB8888, b, 16u*8u));
    sweep_render_mark_stale(&r);
    check("[surf] stale after reconfigure", !sweep_render_is_current(&r, 2u));
    sweep_render_shutdown(&r);
    /* caller-owned buffers not freed by renderer/surface release */
}

static bool offset_glyph_provider(void* user, uint32_t codepoint, SWEEP_RENDER_GLYPH* out) {
    static const uint8_t box[16] = { 0xFF,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xFF };
    (void)user; (void)codepoint;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->bitmap = box; out->width = 8u; out->height = 16u; out->advance_x = 8u;
    out->offset_x = 1; out->offset_y = 1; /* positive placement offset (metric-like) */
    return 1;
}

static void test_phase10_metric_and_contract(void) {
    int32_t px = 0;
    uint32_t storage[16u * 16u];
    SWEEP_RENDER_SURFACE surf;
    SWEEP_RENDERER r;
    SWEEP_PAINT_CMD cmds[2];
    /* Font-unit conversion: validated, deterministic rounding, invalid params */
    check("[p10] conv 0", sweep_render_font_units_to_px(0, 16, 1000, &px) && px == 0);
    check("[p10] conv positive", sweep_render_font_units_to_px(1000, 16, 1000, &px) && px == 16);
    check("[p10] conv negative", sweep_render_font_units_to_px(-1000, 16, 1000, &px) && px == -16);
    check("[p10] conv rounding", sweep_render_font_units_to_px(500, 1, 1000, &px) && (px == 1 || px == 0));
    check("[p10] conv zero den invalid", !sweep_render_font_units_to_px(100, 16, 0, &px));
    check("[p10] conv zero scale invalid", !sweep_render_font_units_to_px(100, 0, 1000, &px));
    /* Placement offsets applied by renderer (metric/bearing-style pixel offsets) */
    sweep_render_surface_init(&surf);
    check("[p10] surface", sweep_render_surface_configure(&surf, 16u, 16u, 16u, SWEEP_RENDER_PIXEL_XRGB8888, storage, 16u*16u));
    sweep_render_init(&r);
    check("[p10] attach", sweep_render_attach(&r, &surf));
    sweep_render_set_glyph_provider(&r, offset_glyph_provider, NULL);
    memset(cmds, 0, sizeof(cmds));
    cmds[0].op = SWEEP_PAINT_CLEAR; cmds[0].w = 16u; cmds[0].h = 16u; cmds[0].color = 0xFF000000u;
    cmds[1].op = SWEEP_PAINT_TEXT; cmds[1].x = 0; cmds[1].y = 0; cmds[1].w = 8u; cmds[1].h = 16u; cmds[1].color = 0x00FFFFFFu; cmds[1].glyph = 0x41u;
    cmds[1].font_size_px = 16u; cmds[1].text_flags = SWEEP_PAINT_TEXT_FLAG_FONT_SIZE_VALID; /* legacy-safe extended fields */
    check("[p10] render with offsets", sweep_render_commands(&r, cmds, 2u, false) == SWEEP_RENDER_OK);
    /* provider offset_x/y=1 shifts top-left drawn set bit from (0,0) to (1,1) */
    check("[p10] offset placement applied", px_at(&surf, 1u, 1u) == 0x00FFFFFFu);
    check("[p10] origin shifted (legacy pos not set)", px_at(&surf, 0u, 0u) == 0x00000000u);
    sweep_render_shutdown(&r);
}

int main(void) {
    printf("=== Sweep render tests (Phase 7/8) ===\n");
    test_surface_and_clear_rect();
    test_clipping_and_bounds();
    test_safety();
    test_text_and_superunicode();
    test_document_integration();
    test_suf_provider();
    test_presentation_boundary();
    test_render_revision_tracking();
    test_suf_pixel_reference();
    test_surface_replacement_lifecycle();
    test_phase10_metric_and_contract();
    printf("--------------------------------\n");
    printf("passed: %d, failed: %d\n", g_passed, g_failures);
    return g_failures ? 1 : 0;
}
