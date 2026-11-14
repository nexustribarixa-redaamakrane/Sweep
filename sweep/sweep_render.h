/* sweep_render.h - Sweep paint-command renderer (Phase 7/8).
 *
 * Consumes validated paint commands (sweep_engine.h / sweep_css layout
 * pipeline) and rasterizes them into a bounded software surface.  This is a
 * software rasterization path for host tests and for integration behind an
 * OpenWindows graphics adapter; it is not a physical display, compositor, GPU
 * backend, or windowing system unless explicitly integrated and demonstrated.
 *
 * Supported paint operations (matching current Sweep paint output): CLEAR,
 * RECT, TEXT.  IMAGE is not emitted by the current paint pipeline and is
 * reported as unsupported by this renderer.  TEXT rendering is separated from
 * character representation via a glyph-provider interface; no glyph raster
 * data is present in paint commands (cmd.glyph is a codepoint value).
 *
 * Phase 8: SUF-backed glyph provider (boot-bitmap path, using existing SUF
 * parser APIs), presentation-adapter boundary/simulation, and render-frame
 * revision tracking for document lifecycle integration.
 */
#ifndef SWEEP_RENDER_H
#define SWEEP_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sweep_engine.h"

#define SWEEP_RENDER_MAX_WIDTH  4096u
#define SWEEP_RENDER_MAX_HEIGHT 4096u
#define SWEEP_RENDER_MAX_PIXELS (4u * 1024u * 1024u)

typedef enum _SWEEP_RENDER_PIXEL_FORMAT {
    SWEEP_RENDER_PIXEL_XRGB8888 = 0, /* 0x00RRGGBB in memory word */
    SWEEP_RENDER_PIXEL_ARGB8888 = 1  /* 0xAARRGGBB in memory word */
} SWEEP_RENDER_PIXEL_FORMAT;

typedef enum _SWEEP_RENDER_STATUS {
    SWEEP_RENDER_OK = 0,
    SWEEP_RENDER_INVALID,
    SWEEP_RENDER_UNSUPPORTED,
    SWEEP_RENDER_OVERFLOW,
    SWEEP_RENDER_TRUNCATED,
    SWEEP_RENDER_NO_SURFACE
} SWEEP_RENDER_STATUS;

typedef struct _SWEEP_RENDER_DIAGNOSTICS {
    uint32_t commands_processed;
    uint32_t clear_commands;
    uint32_t rect_commands;
    uint32_t text_commands;
    uint32_t invalid_commands;
    uint32_t pixels_written;
    uint32_t pixels_clipped; /* pixels skipped by bounds/clip (attempted outside) */
    uint32_t glyph_lookups;
    uint32_t missing_glyphs;
    uint32_t rasterization_failures;
    uint32_t frames_rendered;
} SWEEP_RENDER_DIAGNOSTICS;

typedef struct _SWEEP_RENDER_SURFACE {
    uint32_t* pixels;       /* caller-owned or internal storage */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /* pixels per row */
    uint8_t format;         /* SWEEP_RENDER_PIXEL_FORMAT */
    bool owns_pixels;
} SWEEP_RENDER_SURFACE;

/* Glyph bitmap for text rasterization (1bpp rows, MSB-left, 8 bits/row). */
typedef struct _SWEEP_RENDER_GLYPH {
    const uint8_t* bitmap;  /* 1bpp, row-major, MSB-left, row stride ceil(width/8) */
    uint32_t width;         /* pixels */
    uint32_t height;        /* pixels */
    uint32_t advance_x;     /* placement advance in surface pixels */
    int32_t offset_x;       /* bitmap placement offset from cmd origin (pixels), default 0 */
    int32_t offset_y;       /* bitmap placement offset from cmd origin (pixels), default 0 */
} SWEEP_RENDER_GLYPH;

typedef bool (*SWEEP_RENDER_GLYPH_PROVIDER)(void* user, uint32_t codepoint,
                                            SWEEP_RENDER_GLYPH* out);

typedef struct _SWEEP_RENDERER {
    SWEEP_RENDER_SURFACE surface;
    SWEEP_RENDER_GLYPH_PROVIDER glyph_provider;
    void* glyph_user;
    SWEEP_RENDER_DIAGNOSTICS diag;
    uint32_t render_revision;   /* last successfully rendered paint revision */
    bool frame_current;         /* render_revision matches last requested revision */
    bool initialized;
} SWEEP_RENDERER;

/* ---- SUF-backed glyph provider (Phase 8/10, boot-bitmap path) ----
 * Uses existing SUF parser interfaces from the SuperUnicode font library
 * (suf_validate_header, suf_lookup_glyph_id/_ext, suf_get_boot_bitmap,
 * suf_get_glyph_metric).  Font buffer is caller-owned and must remain valid
 * while the provider is in use.  Character codepoints are mapped to SUF
 * glyph IDs via the font cmap (never assumed equal to glyph IDs).  Missing
 * mappings fall back to glyph 0 (notdef) when available.  Native SUCS values
 * including 0xD800-0xDFFF are preserved as ordinary character identities.
 *
 * Metric conversion (Phase 10): font units -> pixels for intrinsic boot-bitmap
 * placement uses validated scale derived from bitmap dimensions and font metrics
 * (units_per_em/ascender-descender as available). Conversion is overflow-safe,
 * deterministic, and never silently treats arbitrary font units as pixels.
 * Bitmap rendering remains at intrinsic bitmap dimensions (no bitmap scaling). */
typedef struct _SWEEP_SUF_FONT {
    const void* buffer;
    size_t size;
    bool valid;
    uint32_t glyph_count;
    uint8_t boot_bitmap_width;
    uint8_t boot_bitmap_height;
    uint8_t boot_bitmap_bpp;
    uint16_t flags;
    uint16_t units_per_em;
    int16_t ascender;   /* font units, from header */
    int16_t descender;  /* font units, from header */
} SWEEP_SUF_FONT;

/* Deterministic font-unit to pixel conversion.
 * Returns false for invalid parameters (zero divisors, overflow). Rounding:
 * round-half-away-from-zero via (value*scale_num + sign*scale_den/2)/scale_den.
 * scale_num/scale_den define pixels per font unit (e.g. bitmap_height / em_span). */
bool sweep_render_font_units_to_px(int32_t font_units, int32_t scale_num,
                                   int32_t scale_den, int32_t* out_px);

bool sweep_suf_font_init(SWEEP_SUF_FONT* font, const void* buffer, size_t size);
void sweep_suf_font_shutdown(SWEEP_SUF_FONT* font);
bool sweep_suf_glyph_provider(void* user, uint32_t codepoint, SWEEP_RENDER_GLYPH* out);

/* ---- Presentation adapter boundary (Phase 8) ----
 * No general-purpose OpenWindows page-presentation/window/compositor API was
 * identified for Sweep; this is a narrow, host-testable adapter/simulation
 * boundary (not physical display output).  It validates handoff of a software
 * surface frame (dimensions, pitch, format, buffer, revision) and records
 * deterministic presentation outcomes. */
typedef enum _SWEEP_PRESENT_STATUS {
    SWEEP_PRESENT_OK = 0,
    SWEEP_PRESENT_INVALID,
    SWEEP_PRESENT_NO_TARGET,
    SWEEP_PRESENT_FORMAT_UNSUPPORTED,
    SWEEP_PRESENT_FAILED
} SWEEP_PRESENT_STATUS;

typedef struct _SWEEP_PRESENT_DIAGNOSTICS {
    uint32_t attempts;
    uint32_t successes;
    uint32_t failures;
    uint32_t last_revision;
} SWEEP_PRESENT_DIAGNOSTICS;

typedef struct _SWEEP_PRESENT_TARGET {
    bool (*present)(void* user, const SWEEP_RENDER_SURFACE* surface,
                    uint32_t revision, SWEEP_PRESENT_STATUS* out_status);
    void* user;
    bool available;
    bool simulate_failure; /* host-testable simulated failure */
    SWEEP_PRESENT_DIAGNOSTICS diag;
} SWEEP_PRESENT_TARGET;

void sweep_present_target_init(SWEEP_PRESENT_TARGET* target);
void sweep_present_target_shutdown(SWEEP_PRESENT_TARGET* target);
bool sweep_present_target_set_simulated(SWEEP_PRESENT_TARGET* target, bool available,
                                        bool simulate_failure);
SWEEP_PRESENT_STATUS sweep_present_frame(SWEEP_PRESENT_TARGET* target,
                                         const SWEEP_RENDER_SURFACE* surface,
                                         uint32_t revision);
const SWEEP_PRESENT_DIAGNOSTICS* sweep_present_diagnostics(const SWEEP_PRESENT_TARGET* target);

void sweep_render_surface_init(SWEEP_RENDER_SURFACE* surface);
bool sweep_render_surface_configure(SWEEP_RENDER_SURFACE* surface, uint32_t width,
                                    uint32_t height, uint32_t pitch,
                                    SWEEP_RENDER_PIXEL_FORMAT format,
                                    uint32_t* storage, size_t storage_pixels);
void sweep_render_surface_release(SWEEP_RENDER_SURFACE* surface);
uint32_t* sweep_render_surface_pixels(SWEEP_RENDER_SURFACE* surface);
const uint32_t* sweep_render_surface_pixels_const(const SWEEP_RENDER_SURFACE* surface);

void sweep_render_init(SWEEP_RENDERER* renderer);
void sweep_render_shutdown(SWEEP_RENDERER* renderer);
bool sweep_render_attach(SWEEP_RENDERER* renderer, SWEEP_RENDER_SURFACE* surface);
void sweep_render_set_glyph_provider(SWEEP_RENDERER* renderer,
                                     SWEEP_RENDER_GLYPH_PROVIDER provider,
                                     void* user);

/* Render a validated paint-command list into the attached surface.
 * CLEAR resets the covered region; RECT/TEXT are rasterized in command order.
 * Returns SWEEP_RENDER_OK on full success, TRUNCATED if caller indicates
 * truncated input via input_truncated, or an error/unsupported status.
 * Coordinates are paint-command/surface pixel coordinates (no scaling). */
SWEEP_RENDER_STATUS sweep_render_commands(SWEEP_RENDERER* renderer,
                                          const SWEEP_PAINT_CMD* cmds,
                                          size_t count, bool input_truncated);

/* Render and tag the frame with a paint revision for lifecycle tracking.
 * On success/truncation the renderer records render_revision and marks frame
 * current; on hard failure the frame is marked stale. */
SWEEP_RENDER_STATUS sweep_render_commands_revision(SWEEP_RENDERER* renderer,
                                                   const SWEEP_PAINT_CMD* cmds,
                                                   size_t count, bool input_truncated,
                                                   uint32_t paint_revision);
void sweep_render_mark_stale(SWEEP_RENDERER* renderer);
bool sweep_render_is_current(const SWEEP_RENDERER* renderer, uint32_t paint_revision);
uint32_t sweep_render_revision(const SWEEP_RENDERER* renderer);

const SWEEP_RENDER_DIAGNOSTICS* sweep_render_diagnostics(const SWEEP_RENDERER* renderer);
void sweep_render_reset_diagnostics(SWEEP_RENDERER* renderer);

#endif /* SWEEP_RENDER_H */
