/* sweep_engine.h - Sweep browser-engine slice (host-testable, no kernel deps).
 *
 * These are the first browser-engine-specific functions of the Sweep
 * associated kernel: URL admission, viewport negotiation and paint-command
 * encoding.  They are intentionally free of kernel headers so they can be
 * unit-tested in the Sweep tree and later exposed as KDB Service Bridge
 * services for the main kernel.
 */
#ifndef SWEEP_ENGINE_H
#define SWEEP_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWEEP_URL_MAX 1024u
#define SWEEP_PAINT_MAX_CMDS 256u

typedef struct _SWEEP_URL {
    char host[256];
    char path[SWEEP_URL_MAX];
    uint16_t port;
    bool has_port;
} SWEEP_URL;

typedef struct _SWEEP_VIEWPORT {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t device_scale; /* 1 = 1x */
} SWEEP_VIEWPORT;

typedef enum _SWEEP_PAINT_OP {
    SWEEP_PAINT_CLEAR = 1,
    SWEEP_PAINT_RECT,
    SWEEP_PAINT_TEXT,
    SWEEP_PAINT_IMAGE
} SWEEP_PAINT_OP;

/* TEXT flags (Phase 10). Zero-initialized legacy commands remain valid. */
#define SWEEP_PAINT_TEXT_FLAG_BASELINE_VALID 0x0001u
#define SWEEP_PAINT_TEXT_FLAG_FONT_SIZE_VALID 0x0002u

typedef struct _SWEEP_PAINT_CMD {
    uint8_t op;
    uint8_t _pad0;
    uint16_t text_flags;   /* SWEEP_PAINT_TEXT_FLAG_* for TEXT */
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
    uint32_t color;
    uint32_t glyph;        /* codepoint for TEXT */
    uint32_t font_size_px; /* target font size/cell height in px (0 = legacy) */
    int32_t baseline_y;    /* absolute baseline y when BASELINE_VALID; else 0 */
} SWEEP_PAINT_CMD;

/* Parse and admit an http/https URL.  Returns true only for a well-formed
 * absolute URL with a non-empty host; fills OutUrl. */
bool SweepUrlParse(const char* text, SWEEP_URL* out_url);

/* Negotiate a viewport: clamp the request to the engine's supported bounds.
 * Returns false for an empty request. */
bool SweepViewportNegotiate(const SWEEP_VIEWPORT* requested,
                            SWEEP_VIEWPORT* out_viewport);

/* Append a paint command to a bounded list.  Returns false when the list is
 * full or the command is malformed. */
bool SweepPaintAppend(SWEEP_PAINT_CMD* list, size_t capacity, size_t* count,
                      SWEEP_PAINT_CMD cmd);

#endif /* SWEEP_ENGINE_H */
