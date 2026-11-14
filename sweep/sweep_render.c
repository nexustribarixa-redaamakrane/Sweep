/* sweep_render.c - Sweep paint-command software renderer (Phase 7/8). */
#include "sweep_render.h"

#include <string.h>

/* SUF parser (SuperUnicode font, boot-bitmap path). Header path provided by
 * build include dirs when SUF integration is compiled/tested. */
#include "suf/suf_parser.h"
#include "suf/suf_types.h"

void sweep_render_surface_init(SWEEP_RENDER_SURFACE* surface) {
    if (!surface) return;
    memset(surface, 0, sizeof(*surface));
}

static bool mul_overflow_u32(uint32_t a, uint32_t b, uint32_t* out) {
    uint64_t v = (uint64_t)a * (uint64_t)b;
    if (v > 0xFFFFFFFFu) return true;
    *out = (uint32_t)v;
    return false;
}

bool sweep_render_surface_configure(SWEEP_RENDER_SURFACE* surface, uint32_t width,
                                    uint32_t height, uint32_t pitch,
                                    SWEEP_RENDER_PIXEL_FORMAT format,
                                    uint32_t* storage, size_t storage_pixels) {
    uint32_t need = 0u;
    uint32_t row_bytes = 0u;
    if (!surface) return false;
    sweep_render_surface_release(surface);
    if (width == 0u || height == 0u) return false;
    if (width > SWEEP_RENDER_MAX_WIDTH || height > SWEEP_RENDER_MAX_HEIGHT) return false;
    if (pitch == 0u) pitch = width;
    if (pitch < width) return false;
    if (mul_overflow_u32(pitch, height, &need)) return false;
    if (need == 0u || need > SWEEP_RENDER_MAX_PIXELS) return false;
    (void)mul_overflow_u32(need, 4u, &row_bytes); /* overflow checked by MAX_PIXELS */
    if (!storage || storage_pixels < (size_t)need) return false;
    surface->pixels = storage;
    surface->width = width;
    surface->height = height;
    surface->pitch = pitch;
    surface->format = (uint8_t)format;
    surface->owns_pixels = false;
    memset(surface->pixels, 0, (size_t)need * sizeof(uint32_t));
    return true;
}

void sweep_render_surface_release(SWEEP_RENDER_SURFACE* surface) {
    if (!surface) return;
    /* Caller-owned storage in Phase 7 host path; nothing to free. */
    surface->pixels = NULL;
    surface->width = 0u;
    surface->height = 0u;
    surface->pitch = 0u;
    surface->format = 0u;
    surface->owns_pixels = false;
}

uint32_t* sweep_render_surface_pixels(SWEEP_RENDER_SURFACE* surface) {
    return surface ? surface->pixels : NULL;
}

const uint32_t* sweep_render_surface_pixels_const(const SWEEP_RENDER_SURFACE* surface) {
    return surface ? surface->pixels : NULL;
}

void sweep_render_init(SWEEP_RENDERER* renderer) {
    if (!renderer) return;
    memset(renderer, 0, sizeof(*renderer));
    renderer->initialized = true;
    renderer->frame_current = false;
    renderer->render_revision = 0u;
}

void sweep_render_shutdown(SWEEP_RENDERER* renderer) {
    if (!renderer) return;
    renderer->surface.pixels = NULL; /* do not free caller storage */
    renderer->glyph_provider = NULL;
    renderer->glyph_user = NULL;
    renderer->frame_current = false;
    renderer->render_revision = 0u;
    renderer->initialized = false;
}

bool sweep_render_attach(SWEEP_RENDERER* renderer, SWEEP_RENDER_SURFACE* surface) {
    if (!renderer || !renderer->initialized || !surface) return false;
    if (!surface->pixels || surface->width == 0u || surface->height == 0u) return false;
    renderer->surface = *surface;
    renderer->surface.owns_pixels = false;
    return true;
}

void sweep_render_set_glyph_provider(SWEEP_RENDERER* renderer,
                                     SWEEP_RENDER_GLYPH_PROVIDER provider,
                                     void* user) {
    if (!renderer) return;
    renderer->glyph_provider = provider;
    renderer->glyph_user = user;
}

const SWEEP_RENDER_DIAGNOSTICS* sweep_render_diagnostics(const SWEEP_RENDERER* renderer) {
    return renderer ? &renderer->diag : NULL;
}

void sweep_render_reset_diagnostics(SWEEP_RENDERER* renderer) {
    if (!renderer) return;
    memset(&renderer->diag, 0, sizeof(renderer->diag));
}

static uint32_t compose_color(uint32_t cmd_color, SWEEP_RENDER_PIXEL_FORMAT fmt) {
    /* Paint commands use 0xAARRGGBB. XRGB8888 ignores alpha in storage word. */
    if (fmt == SWEEP_RENDER_PIXEL_XRGB8888) {
        return cmd_color & 0x00FFFFFFu;
    }
    return cmd_color;
}

static bool clip_rect(const SWEEP_RENDER_SURFACE* s, int32_t x, int32_t y,
                      uint32_t w, uint32_t h,
                      uint32_t* ox, uint32_t* oy, uint32_t* ow, uint32_t* oh,
                      uint32_t* clipped_pixels) {
    int64_t x0 = x, y0 = y, x1 = x + (int64_t)w, y1 = y + (int64_t)h;
    int64_t cx0, cy0, cx1, cy1;
    uint64_t orig = (uint64_t)w * (uint64_t)h;
    uint64_t vis;
    if (!s || w == 0u || h == 0u) return false;
    cx0 = x0 < 0 ? 0 : x0;
    cy0 = y0 < 0 ? 0 : y0;
    cx1 = x1 > (int64_t)s->width ? (int64_t)s->width : x1;
    cy1 = y1 > (int64_t)s->height ? (int64_t)s->height : y1;
    if (cx1 <= cx0 || cy1 <= cy0) {
        if (clipped_pixels) {
            *clipped_pixels = orig > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)orig;
        }
        return false;
    }
    *ox = (uint32_t)cx0; *oy = (uint32_t)cy0;
    *ow = (uint32_t)(cx1 - cx0); *oh = (uint32_t)(cy1 - cy0);
    vis = (uint64_t)(*ow) * (uint64_t)(*oh);
    if (clipped_pixels) {
        uint64_t c = orig > vis ? (orig - vis) : 0u;
        *clipped_pixels = c > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)c;
    }
    return true;
}

static void fill_rect(SWEEP_RENDER_SURFACE* s, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t color,
                      SWEEP_RENDER_DIAGNOSTICS* d) {
    uint32_t row, col;
    uint32_t px = compose_color(color, (SWEEP_RENDER_PIXEL_FORMAT)s->format);
    for (row = 0u; row < h; ++row) {
        uint32_t* dst = s->pixels + (size_t)(y + row) * s->pitch + x;
        for (col = 0u; col < w; ++col) dst[col] = px;
    }
    if (d) {
        uint64_t n = (uint64_t)w * (uint64_t)h + (uint64_t)d->pixels_written;
        d->pixels_written = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
    }
}

static SWEEP_RENDER_STATUS render_clear(SWEEP_RENDERER* r, const SWEEP_PAINT_CMD* c) {
    uint32_t x, y, w, h, clipped = 0u;
    /* CLEAR uses command rect (viewport in generated paint). */
    if (c->w == 0u || c->h == 0u) return SWEEP_RENDER_INVALID;
    if (!clip_rect(&r->surface, c->x, c->y, c->w, c->h, &x, &y, &w, &h, &clipped)) {
        r->diag.pixels_clipped += clipped;
        r->diag.clear_commands++;
        return SWEEP_RENDER_OK;
    }
    r->diag.pixels_clipped += clipped;
    fill_rect(&r->surface, x, y, w, h, c->color, &r->diag);
    r->diag.clear_commands++;
    return SWEEP_RENDER_OK;
}

static SWEEP_RENDER_STATUS render_rect(SWEEP_RENDERER* r, const SWEEP_PAINT_CMD* c) {
    uint32_t x, y, w, h, clipped = 0u;
    if (c->w == 0u || c->h == 0u) { r->diag.rect_commands++; return SWEEP_RENDER_OK; }
    if ((c->color >> 24) == 0u) { r->diag.rect_commands++; return SWEEP_RENDER_OK; }
    if (!clip_rect(&r->surface, c->x, c->y, c->w, c->h, &x, &y, &w, &h, &clipped)) {
        r->diag.pixels_clipped += clipped;
        r->diag.rect_commands++;
        return SWEEP_RENDER_OK;
    }
    r->diag.pixels_clipped += clipped;
    fill_rect(&r->surface, x, y, w, h, c->color, &r->diag);
    r->diag.rect_commands++;
    return SWEEP_RENDER_OK;
}

static SWEEP_RENDER_STATUS render_text(SWEEP_RENDERER* r, const SWEEP_PAINT_CMD* c) {
    SWEEP_RENDER_GLYPH g;
    uint32_t x, y, w, h, clipped = 0u;
    uint32_t row, col;
    uint32_t px;

    r->diag.text_commands++;
    if (!r->glyph_provider) {
        r->diag.missing_glyphs++;
        return SWEEP_RENDER_UNSUPPORTED;
    }
    if (c->w == 0u || c->h == 0u || c->glyph == 0u) return SWEEP_RENDER_INVALID;
    r->diag.glyph_lookups++;
    memset(&g, 0, sizeof(g));
    if (!r->glyph_provider(r->glyph_user, c->glyph, &g) || !g.bitmap || g.width == 0u || g.height == 0u) {
        r->diag.missing_glyphs++;
        return SWEEP_RENDER_OK; /* missing glyph: deterministic no-op draw */
    }
    if (!clip_rect(&r->surface, c->x, c->y, c->w, c->h, &x, &y, &w, &h, &clipped)) {
        r->diag.pixels_clipped += clipped;
        return SWEEP_RENDER_OK;
    }
    r->diag.pixels_clipped += clipped;
    px = compose_color(c->color, (SWEEP_RENDER_PIXEL_FORMAT)r->surface.format);

    /* Rasterize 1bpp glyph bitmap into the command's (clipped) cell region.
     * Bitmap placement may include a provider-supplied pixel offset (default 0)
     * relative to the paint command origin (advance cell). Offsets are applied
     * before clipping; invalid/out-of-bounds samples are skipped safely. */
    {
        int32_t origin_x = c->x + g.offset_x;
        int32_t origin_y = c->y + g.offset_y;
        for (row = 0u; row < h; ++row) {
            int32_t sy = (int32_t)(y + row);
            int32_t gy_i = sy - origin_y;
            uint32_t gy;
            const uint8_t* bits_row;
            if (gy_i < 0) continue;
            gy = (uint32_t)gy_i;
            if (gy >= g.height) continue;
            bits_row = &g.bitmap[gy * ((g.width + 7u) / 8u)];
            for (col = 0u; col < w; ++col) {
                int32_t sx = (int32_t)(x + col);
                int32_t gx_i = sx - origin_x;
                uint32_t gx;
                uint8_t bits;
                if (gx_i < 0) continue;
                gx = (uint32_t)gx_i;
                if (gx >= g.width) continue;
                bits = bits_row[gx >> 3];
                if ((bits & (uint8_t)(0x80u >> (gx & 7u))) != 0u) {
                    r->surface.pixels[(size_t)(y + row) * r->surface.pitch + (x + col)] = px;
                    if (r->diag.pixels_written != 0xFFFFFFFFu) r->diag.pixels_written++;
                }
            }
        }
    }
    return SWEEP_RENDER_OK;
}

SWEEP_RENDER_STATUS sweep_render_commands(SWEEP_RENDERER* renderer,
                                          const SWEEP_PAINT_CMD* cmds,
                                          size_t count, bool input_truncated) {
    size_t i;
    if (!renderer || !renderer->initialized) return SWEEP_RENDER_INVALID;
    if (!renderer->surface.pixels || renderer->surface.width == 0u ||
        renderer->surface.height == 0u) {
        renderer->frame_current = false;
        return SWEEP_RENDER_NO_SURFACE;
    }
    if (!cmds && count > 0u) {
        renderer->frame_current = false;
        return SWEEP_RENDER_INVALID;
    }

    for (i = 0u; i < count; ++i) {
        const SWEEP_PAINT_CMD* c = &cmds[i];
        SWEEP_RENDER_STATUS st = SWEEP_RENDER_OK;
        renderer->diag.commands_processed++;
        switch (c->op) {
        case SWEEP_PAINT_CLEAR:
            st = render_clear(renderer, c);
            break;
        case SWEEP_PAINT_RECT:
            st = render_rect(renderer, c);
            break;
        case SWEEP_PAINT_TEXT:
            st = render_text(renderer, c);
            break;
        case SWEEP_PAINT_IMAGE:
            renderer->diag.invalid_commands++;
            st = SWEEP_RENDER_UNSUPPORTED;
            break;
        default:
            renderer->diag.invalid_commands++;
            st = SWEEP_RENDER_INVALID;
            break;
        }
        if (st == SWEEP_RENDER_INVALID || st == SWEEP_RENDER_OVERFLOW) {
            renderer->frame_current = false;
            return st;
        }
        /* unsupported individual ops (missing provider/glyph, image) recorded but
         * do not abort the frame; deterministic best-effort. */
    }
    renderer->diag.frames_rendered++;
    if (input_truncated) return SWEEP_RENDER_TRUNCATED;
    return SWEEP_RENDER_OK;
}

SWEEP_RENDER_STATUS sweep_render_commands_revision(SWEEP_RENDERER* renderer,
                                                   const SWEEP_PAINT_CMD* cmds,
                                                   size_t count, bool input_truncated,
                                                   uint32_t paint_revision) {
    SWEEP_RENDER_STATUS st = sweep_render_commands(renderer, cmds, count, input_truncated);
    if (!renderer) return st;
    if (st == SWEEP_RENDER_OK || st == SWEEP_RENDER_TRUNCATED) {
        renderer->render_revision = paint_revision;
        renderer->frame_current = true;
    } else {
        renderer->frame_current = false;
    }
    return st;
}

void sweep_render_mark_stale(SWEEP_RENDERER* renderer) {
    if (!renderer) return;
    renderer->frame_current = false;
}

bool sweep_render_is_current(const SWEEP_RENDERER* renderer, uint32_t paint_revision) {
    if (!renderer || !renderer->initialized) return false;
    return renderer->frame_current && renderer->render_revision == paint_revision;
}

uint32_t sweep_render_revision(const SWEEP_RENDERER* renderer) {
    return renderer ? renderer->render_revision : 0u;
}

/* ---- SUF glyph provider (Phase 8) ---- */

bool sweep_render_font_units_to_px(int32_t font_units, int32_t scale_num,
                                   int32_t scale_den, int32_t* out_px) {
    int64_t prod, adj, res;
    if (!out_px) return false;
    *out_px = 0;
    if (scale_den == 0 || scale_num <= 0) return false;
    prod = (int64_t)font_units * (int64_t)scale_num;
    /* round half away from zero */
    if (prod >= 0) adj = (int64_t)scale_den / 2;
    else adj = -((int64_t)scale_den / 2);
    res = (prod + adj) / (int64_t)scale_den;
    if (res < -2147483647LL || res > 2147483647LL) return false;
    *out_px = (int32_t)res;
    return true;
}

bool sweep_suf_font_init(SWEEP_SUF_FONT* font, const void* buffer, size_t size) {
    suf_header_t hdr;
    if (!font) return false;
    memset(font, 0, sizeof(*font));
    if (!buffer || size == 0u) return false;
    if (suf_validate_header(buffer, size, &hdr) != SUF_OK) return false;
    if (!(hdr.flags & SUF_FLAG_BOOT_BITMAP) || hdr.boot_bitmap_size == 0u) return false;
    if (hdr.boot_bitmap_width == 0u || hdr.boot_bitmap_height == 0u) return false;
    if (hdr.boot_bitmap_bpp != 1u && hdr.boot_bitmap_bpp != 8u) return false;
    if (hdr.units_per_em == 0u) return false;
    font->buffer = buffer;
    font->size = size;
    font->valid = true;
    font->glyph_count = hdr.glyph_count;
    font->boot_bitmap_width = hdr.boot_bitmap_width;
    font->boot_bitmap_height = hdr.boot_bitmap_height;
    font->boot_bitmap_bpp = hdr.boot_bitmap_bpp;
    font->flags = hdr.flags;
    font->units_per_em = hdr.units_per_em;
    font->ascender = hdr.ascender;
    font->descender = hdr.descender;
    return true;
}

void sweep_suf_font_shutdown(SWEEP_SUF_FONT* font) {
    if (!font) return;
    /* buffer is caller-owned; do not free. */
    memset(font, 0, sizeof(*font));
}

bool sweep_suf_glyph_provider(void* user, uint32_t codepoint, SWEEP_RENDER_GLYPH* out) {
    SWEEP_SUF_FONT* font = (SWEEP_SUF_FONT*)user;
    uint32_t glyph_id = 0u;
    const uint8_t* bitmap = NULL;
    size_t byte_count = 0u;
    suf_metric_t metric;
    suf_status_t st;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!font || !font->valid || !font->buffer) return false;

    /* Character -> glyph mapping via SUF cmap (never assume codepoint == glyph ID).
     * Native SUCS values including 0xD800-0xDFFF are preserved as ordinary
     * character identities and looked up as-is. */
    st = suf_lookup_glyph_id(font->buffer, font->size, codepoint, &glyph_id);
    if (st != SUF_OK) {
        /* Deterministic notdef fallback: glyph 0 when present. Missing mapping
         * does not invalidate the character value. */
        glyph_id = 0u;
    }
    if (glyph_id >= font->glyph_count) glyph_id = 0u;

    st = suf_get_boot_bitmap(font->buffer, font->size, glyph_id, &bitmap, &byte_count);
    if (st != SUF_OK || !bitmap || byte_count == 0u) {
        /* Try notdef if a different glyph failed unexpectedly. */
        if (glyph_id != 0u) {
            glyph_id = 0u;
            st = suf_get_boot_bitmap(font->buffer, font->size, glyph_id, &bitmap, &byte_count);
        }
        if (st != SUF_OK || !bitmap || byte_count == 0u) return false;
    }

    out->bitmap = bitmap;
    out->width = (uint32_t)font->boot_bitmap_width;
    out->height = (uint32_t)font->boot_bitmap_height;
    out->offset_x = 0;
    out->offset_y = 0;

    /* Metric-aware handling for intrinsic boot bitmap (no bitmap scaling).
     * Horizontal placement: if left_side_bearing is valid, convert font units
     * to pixels using deterministic scale bitmap_width/units_per_em (intrinsic
     * bitmap space) with overflow-safe rounding. This shifts the bitmap within
     * the advance cell relative to command origin. Vertical baseline/bearing
     * placement is not applied without explicit baseline context in paint cmd.
     * Advance: use metric advance_width when in sane bitmap-pixel range, else
     * fallback to bitmap width (do not silently treat arbitrary font units as
     * pixels). */
    if (suf_get_glyph_metric(font->buffer, font->size, glyph_id, &metric) == SUF_OK) {
        int32_t lsb_px = 0;
        if (font->units_per_em != 0u &&
            sweep_render_font_units_to_px(metric.left_side_bearing,
                                          (int32_t)out->width,
                                          (int32_t)font->units_per_em, &lsb_px)) {
            /* clamp to reasonable range relative to bitmap/cell */
            if (lsb_px > (int32_t)out->width) lsb_px = (int32_t)out->width;
            if (lsb_px < -((int32_t)out->width)) lsb_px = -((int32_t)out->width);
            out->offset_x = lsb_px;
        }
        if (metric.advance_width > 0 &&
            (uint32_t)metric.advance_width <= (out->width * 4u + 8u)) {
            out->advance_x = (uint32_t)metric.advance_width;
        } else {
            out->advance_x = out->width;
        }
    } else {
        out->advance_x = out->width;
    }
    return true;
    return true;
}

/* ---- Presentation adapter boundary/simulation (Phase 8) ---- */

void sweep_present_target_init(SWEEP_PRESENT_TARGET* target) {
    if (!target) return;
    memset(target, 0, sizeof(*target));
}

void sweep_present_target_shutdown(SWEEP_PRESENT_TARGET* target) {
    if (!target) return;
    memset(target, 0, sizeof(*target));
}

bool sweep_present_target_set_simulated(SWEEP_PRESENT_TARGET* target, bool available,
                                        bool simulate_failure) {
    if (!target) return false;
    target->present = NULL;
    target->user = NULL;
    target->available = available;
    target->simulate_failure = simulate_failure;
    return true;
}

SWEEP_PRESENT_STATUS sweep_present_frame(SWEEP_PRESENT_TARGET* target,
                                         const SWEEP_RENDER_SURFACE* surface,
                                         uint32_t revision) {
    SWEEP_PRESENT_STATUS st = SWEEP_PRESENT_OK;
    if (!target) return SWEEP_PRESENT_INVALID;
    target->diag.attempts++;
    if (!target->available) {
        target->diag.failures++;
        return SWEEP_PRESENT_NO_TARGET;
    }
    if (!surface || !surface->pixels || surface->width == 0u || surface->height == 0u ||
        surface->pitch < surface->width) {
        target->diag.failures++;
        return SWEEP_PRESENT_INVALID;
    }
    if (surface->format != (uint8_t)SWEEP_RENDER_PIXEL_XRGB8888 &&
        surface->format != (uint8_t)SWEEP_RENDER_PIXEL_ARGB8888) {
        target->diag.failures++;
        return SWEEP_PRESENT_FORMAT_UNSUPPORTED;
    }
    if (target->present) {
        if (!target->present(target->user, surface, revision, &st)) {
            if (st == SWEEP_PRESENT_OK) st = SWEEP_PRESENT_FAILED;
            target->diag.failures++;
            return st;
        }
    } else if (target->simulate_failure) {
        target->diag.failures++;
        return SWEEP_PRESENT_FAILED;
    }
    target->diag.successes++;
    target->diag.last_revision = revision;
    return SWEEP_PRESENT_OK;
}

const SWEEP_PRESENT_DIAGNOSTICS* sweep_present_diagnostics(const SWEEP_PRESENT_TARGET* target) {
    return target ? &target->diag : NULL;
}
