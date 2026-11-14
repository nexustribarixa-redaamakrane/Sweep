# Sweep Rendering Backend (Phase 7)

Status: **Partially implemented (host-testable software rasterization)**, verified 2026-11-10.
Scope: consume existing validated Sweep paint commands and rasterize them into a bounded
software surface for deterministic host tests and as an integration boundary for OpenWindows
graphics. No physical display presentation, no compositor, no window system, no GPU/hardware
acceleration, and no incremental paint are implemented or claimed.

## 1. Audited existing architecture

### 1.1 Sweep paint-command contract (existing, consumed as-is)
- **Source:** `sweep/sweep_engine.h`, `sweep/sweep_css.h/c` (`sweep_paint_from_layout`),
  `sweep/sweep_document.h/c`.
- **Types:** `SWEEP_PAINT_CMD { uint8_t op; int32_t x,y; uint32_t w,h; uint32_t color; uint32_t glyph; }`.
  `SWEEP_PAINT_MAX_CMDS` = 256.
- **Ops:** `SWEEP_PAINT_CLEAR` (1), `SWEEP_PAINT_RECT` (2), `SWEEP_PAINT_TEXT` (3),
  `SWEEP_PAINT_IMAGE` (4, defined but not emitted by current paint pipeline).
- **Generation/ordering:** `sweep_paint_from_layout` emits one CLEAR (viewport-sized, white
  `0xFFFFFFFF`) then box paints in depth-first/ascending box-index order (ancestor background
  before descendant content). Borders/backgrounds emitted as RECTs; text as per-codepoint TEXT
  commands.
- **Coordinates/units:** integer pixel-space coordinates/sizes derived from layout and viewport
  (`SWEEP_VIEWPORT` px). Command rects are pre-clipped/intersected against viewport during
  paint generation where applicable; renderer applies additional surface-bounds clipping.
- **Color:** `0xAARRGGBB`. Transparent alpha (`>>24 == 0`) is skipped for RECT/text emission.
- **TEXT payload:** `cmd.glyph` is the decoded codepoint value (uint32_t) from node text under
  the document/layout text mode (`sweep_utf8_decode`); `x,y,w,h` represent the clipped advance
  cell for that glyph placement (not glyph bitmap metrics). No glyph raster/outline/bitmap data
  is carried in commands. Deterministic fixed-advance measurement exists in layout (`sweep_text_measure_px`);
  it is not font shaping.
- **Capacity/truncation:** bounded command emission; overflow reported with actual emitted count
  (`paint_truncated` in document stats / `sweep_paint_from_layout` return).

### 1.2 OpenWindows graphics/font facilities discovered
- **Text-plane rasterization:** `inc/ow_superunicode.h` / `lib/ow_superunicode.c` provide
  `OwSuperUnicodeRenderPlane(uint32_t *framebuffer, uint32_t width, height, pitch, fg, bg)` which
  rasterizes the live SCTS text plane into a caller-supplied **32bpp XRGB pixel buffer** with
  explicit `pitch` (pixels/row), using embedded SUF boot bitmap (8x16, 1bpp) and SUF parser
  helpers (`suf_lookup_glyph_id`, `suf_render_boot_glyph_to_fb`). Fallback to notdef glyph for
  unmapped codepoints.
- **SUF parser:** `suf_parser.h/c` (ecosystem, `../OpenWindows-Essentials/vendor/superunicode/suf`)
  exposes `suf_validate_header`, glyph lookup, boot-bitmap access, and
  `suf_render_boot_glyph_to_fb(..., uint32_t *fb, fb_width, fb_height, fb_pitch_pixels, x, y, fg, bg)`.
  This is a relevant existing glyph-raster primitive for boot-bitmap fonts.
- **Other:** VGA text-mode drawing in HAL (`OwHalVgaDrawText`, etc.). No general-purpose
  framebuffer/display driver/surface/window/compositor/double-buffer/presentation API for
  arbitrary 2D rendering was identified in kernel headers during this audit; physical display
  integration for Sweep page pixels is therefore not implemented.

## 2. Renderer architecture (implemented)

**Modules:** `sweep/sweep_render.h`, `sweep/sweep_render.c` (host-testable, no kernel deps).
**Tests:** `sweep/tests/sweep_render_test.c` (55 checks), wired into `make sweep-tests` as
`sweep_render_test`.

- **Separation:** renderer consumes validated paint commands only; it does not parse HTML/CSS,
  execute JS, mutate DOM, fetch resources, or access document internals. It can be fed by
  `sweep_document_paint` / `sweep_paint_from_layout` output.
- **Interface:** init/shutdown, surface attach/configure/release (caller-owned storage supported),
  glyph-provider injection, `sweep_render_commands(cmds,count,input_truncated)`, diagnostics.
- **Status model:** `OK`, `INVALID`, `UNSUPPORTED`, `OVERFLOW`, `NO_SURFACE`, `TRUNCATED`.
  Unsupported/missing per-command operations (e.g. IMAGE, TEXT without provider/missing glyph)
  are recorded in diagnostics; frame rendering is best-effort/deterministic rather than aborting
  valid CLEAR/RECT work.

## 3. Surface representation and pixel formats

- `SWEEP_RENDER_SURFACE`: `uint32_t* pixels`, `width`, `height`, `pitch` (pixels/row), `format`,
  caller-owned storage flag. Configuration validates dimensions, pitch>=width, overflow-safe
  size, and bounded maxima (`SWEEP_RENDER_MAX_WIDTH/HEIGHT/PIXELS`).
- **Implemented formats:** `SWEEP_RENDER_PIXEL_XRGB8888` (`0x00RRGGBB` stored),
  `SWEEP_RENDER_PIXEL_ARGB8888` (`0xAARRGGBB` stored). No additional formats claimed.
- **Compatibility boundary:** 32bpp word-per-pixel layout with explicit pitch is compatible in
  shape with `OwSuperUnicodeRenderPlane` / `suf_render_boot_glyph_to_fb` framebuffer parameters;
  an OpenWindows adapter can target such a buffer when a surface/framebuffer is provided by the
  platform. No such presentation adapter is implemented here.

## 4. Rasterization (implemented)

- **CLEAR:** fills the command rect region (clipped to surface bounds) with command color.
- **RECT:** fills clipped rectangle; transparent commands (`alpha==0`) and empty rects are no-ops.
  Overlapping commands follow paint order (later overwrites earlier).
- **TEXT:** rasterized only when a glyph provider is attached. Provider returns a 1bpp bitmap
  (`SWEEP_RENDER_GLYPH`: bitmap/width/height/advance). Foreground pixels are written with
  command color; background is not filled (matches per-glyph paint semantics). Glyph bitmap is
  placed at command origin and clipped to command/surface intersection. Missing glyph/provider:
  deterministic no-op draw with `missing_glyphs` counted.
- **IMAGE:** not emitted by current pipeline; renderer records `invalid_commands`/unsupported and
  does not claim support.
- **Determinism/bounds:** integer pixel loops, bounds-checked clipping, overflow-safe size checks,
  no out-of-bounds writes, no unbounded allocation in renderer path.

## 5. Clipping and coordinates

- **Coordinate semantics:** paint-command/surface pixel coordinates in the same space as
  `sweep_paint_from_layout` output and viewport (no scaling, no device-pixel-ratio transform
  introduced). `cmd.x/y` may be negative; clipping handles negative/out-of-range and
  right/bottom overflow. `pixels_clipped` counts attempted pixels outside the visible clipped
  region (best-effort diagnostic).
- **No silent transforms:** layout→paint coordinates are preserved; renderer does not rescale.

## 6. Text/glyph pipeline

- **Explicit separation (implemented interfaces):** (1) text source bytes in DOM,
  (2) character/codepoint decoding in paint generation (`sweep_utf8_decode` under text mode),
  (3) glyph selection/raster data via `SWEEP_RENDER_GLYPH_PROVIDER(codepoint -> bitmap)` at
  render time, (4) glyph rasterization/composition to surface pixels. Codepoint values in paint
  commands are **not** assumed to equal font glyph IDs except by provider convention.
- **Test glyph provider (implemented, deterministic, host tests):** supplies fixed 8x16 1bpp
  patterns for probe codepoints (including `0xD800`) and a fallback pattern; explicitly reports
  missing for a probe. This verifies pipeline separation, lookup/missing behavior, clipping, and
  SuperUnicode codepoint handling — it is **not** a complete font/shaping/glyph-rasterization
  implementation and must not be described as such.
- **SUF/OpenWindows glyph integration:** `suf_render_boot_glyph_to_fb` and embedded SUF boot
  bitmap infrastructure exist in OpenWindows/ecosystem and are compatible targets for a future
  SUF-backed `SWEEP_RENDER_GLYPH_PROVIDER`/adapter; not wired into Sweep renderer in this change.

## 7. SuperUnicode handling

- **Preserved:** renderer treats `cmd.glyph` as an opaque codepoint value; native SUCS PUA
  `0xD800–0xDFFF` are passed through unchanged to the glyph provider and rasterized like any
  other value when mapped (verified in tests for `0xD800`). No UTF-16 surrogate pairing,
  rejection, or reinterpretation is introduced in native rendering. WEB_COMPAT/native distinction
  remains at text decode/paint-generation boundaries (unchanged).
- **Not conflated:** character values != glyph identifiers != font encodings != pixels.

## 8. Compositing, display integration, and lifecycle

- **Implemented:** host-testable software surface rasterization; document→layout→paint→renderer
  integration demonstrated in tests by consuming generated paint commands.
- **Not implemented:** OpenWindows surface/window presentation, compositor integration, buffer
  swap/double buffering, vsync/timing, physical display output, hardware acceleration. A clean
  boundary exists (render to `uint32_t` pixel buffer with pitch/format); actual presentation
  requires platform display/surface APIs not established in this phase.
- **Lifecycle:** renderer is independent of document/JS; callers submit regenerated paint-command
  lists (full paint regeneration remains current behavior). Renderer shutdown clears references to
  attached/caller storage without freeing caller buffers.

## 9. Diagnostics

Deterministic counters: `commands_processed`, `clear_commands`, `rect_commands`, `text_commands`,
`invalid_commands`, `pixels_written` (saturated), `pixels_clipped`, `glyph_lookups`,
`missing_glyphs`, `rasterization_failures`, `frames_rendered`.

## 10. Security and limits

- No JS/website access to renderer, surfaces, framebuffers, or devices (renderer is trusted host
  component). No filesystem/network/KDB exposure added. Bounded dimensions/pixels/loops,
  overflow-safe sizing, no OOB writes, caller-owned storage support, explicit lifecycle.

## 11. Distinct capabilities (do not conflate)
1. **Paint-command generation** — existing (Sweep layout/CSS pipeline).
2. **Software rasterization** — implemented (CLEAR/RECT + TEXT via glyph provider interface).
3. **Glyph rasterization** — interface + deterministic test provider implemented; real font/SUF
   glyph rasterization integration not implemented.
4. **Surface composition** — basic command-order overdraw in one software surface; no compositor.
5. **Display presentation** — not implemented.
6. **Hardware acceleration** — not implemented.
