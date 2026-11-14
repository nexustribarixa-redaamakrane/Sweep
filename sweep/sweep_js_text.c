/* sweep_js_text.c - CESU-8/UTF-8 boundary conversion with explicit modes. */
#include "sweep_js_text.h"

typedef struct _SWEEP_JS_OUT {
    uint8_t* out;
    size_t cap;
    size_t used;
    bool ok;
} SWEEP_JS_OUT;

static void out_byte(SWEEP_JS_OUT* o, uint8_t b) {
    if (!o->ok) return;
    if (o->used >= o->cap) { o->ok = false; return; }
    o->out[o->used++] = b;
}

static void out_cp(SWEEP_JS_OUT* o, uint32_t cp, bool allow_surrogate) {
    if (!o->ok) return;
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu && !allow_surrogate)) {
        cp = SWEEP_CP_REPLACEMENT;
    }
    if (cp < 0x80u) {
        out_byte(o, (uint8_t) cp);
    } else if (cp < 0x800u) {
        out_byte(o, (uint8_t) (0xC0u | (cp >> 6)));
        out_byte(o, (uint8_t) (0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out_byte(o, (uint8_t) (0xE0u | (cp >> 12)));
        out_byte(o, (uint8_t) (0x80u | ((cp >> 6) & 0x3Fu)));
        out_byte(o, (uint8_t) (0x80u | (cp & 0x3Fu)));
    } else {
        out_byte(o, (uint8_t) (0xF0u | (cp >> 18)));
        out_byte(o, (uint8_t) (0x80u | ((cp >> 12) & 0x3Fu)));
        out_byte(o, (uint8_t) (0x80u | ((cp >> 6) & 0x3Fu)));
        out_byte(o, (uint8_t) (0x80u | (cp & 0x3Fu)));
    }
    if (!o->ok) return;
}

static bool is_cont(uint8_t b) { return (b & 0xC0u) == 0x80u; }

/* Decode one extended-UTF-8 sequence.  Surrogates are accepted when
 * allow_surrogate is true.  Invalid input consumes one byte and yields U+FFFD. */
static uint32_t decode_one(const uint8_t* in, size_t len, size_t* used,
                           bool allow_surrogate) {
    uint8_t b0;
    uint32_t cp;
    if (len == 0u) { *used = 0u; return SWEEP_CP_REPLACEMENT; }
    b0 = in[0];
    if (b0 < 0x80u) { *used = 1u; return b0; }
    if (b0 >= 0xC2u && b0 <= 0xDFu && len >= 2u && is_cont(in[1])) {
        *used = 2u;
        return ((uint32_t) (b0 & 0x1Fu) << 6) | (uint32_t) (in[1] & 0x3Fu);
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu && len >= 3u &&
        is_cont(in[1]) && is_cont(in[2])) {
        cp = ((uint32_t) (b0 & 0x0Fu) << 12) |
             ((uint32_t) (in[1] & 0x3Fu) << 6) |
             (uint32_t) (in[2] & 0x3Fu);
        if (cp < 0x800u) { *used = 1u; return SWEEP_CP_REPLACEMENT; }
        if (cp >= 0xD800u && cp <= 0xDFFFu && !allow_surrogate) {
            *used = 1u;
            return SWEEP_CP_REPLACEMENT;
        }
        *used = 3u;
        return cp;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u && len >= 4u &&
        is_cont(in[1]) && is_cont(in[2]) && is_cont(in[3])) {
        cp = ((uint32_t) (b0 & 0x07u) << 18) |
             ((uint32_t) (in[1] & 0x3Fu) << 12) |
             ((uint32_t) (in[2] & 0x3Fu) << 6) |
             (uint32_t) (in[3] & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) {
            *used = 1u;
            return SWEEP_CP_REPLACEMENT;
        }
        *used = 4u;
        return cp;
    }
    *used = 1u;
    return SWEEP_CP_REPLACEMENT;
}

static bool is_high(uint32_t cp) { return cp >= 0xD800u && cp <= 0xDBFFu; }
static bool is_low(uint32_t cp) { return cp >= 0xDC00u && cp <= 0xDFFFu; }

bool sweep_js_cesu8_to_utf8(SWEEP_TEXT_MODE mode, const uint8_t* in,
                            size_t in_len, uint8_t* out, size_t out_cap,
                            size_t* out_len) {
    SWEEP_JS_OUT o;
    size_t i = 0u;
    o.out = out; o.cap = out_cap; o.used = 0u; o.ok = true;
    if (!out && out_cap != 0u) return false;
    while (i < in_len) {
        size_t used = 0u;
        uint32_t cp = decode_one(in + i, in_len - i, &used, true);
        if (used == 0u) break;
        i += used;
        if (mode == SWEEP_TEXT_WEB_COMPAT && is_high(cp) &&
            i < in_len) {
            size_t used2 = 0u;
            uint32_t lo = decode_one(in + i, in_len - i, &used2, true);
            if (is_low(lo)) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += used2;
                out_cp(&o, cp, false);
                continue;
            }
        }
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            if (mode == SWEEP_TEXT_WEB_COMPAT) out_cp(&o, SWEEP_CP_REPLACEMENT, false);
            else out_cp(&o, cp, true); /* native SUCS value preserved */
        } else if (cp > 0xFFFFu && mode == SWEEP_TEXT_NATIVE_SU) {
            /* Native 16-bit engine strings cannot carry a >U+FFFF SUCS as a
             * single unit; do not silently surrogate-pair it in native mode. */
            out_cp(&o, SWEEP_CP_REPLACEMENT, false);
        } else {
            out_cp(&o, cp, false);
        }
    }
    if (out_len) *out_len = o.used;
    return o.ok;
}

bool sweep_js_utf8_to_cesu8(SWEEP_TEXT_MODE mode, const uint8_t* in,
                            size_t in_len, uint8_t* out, size_t out_cap,
                            size_t* out_len) {
    SWEEP_JS_OUT o;
    size_t i = 0u;
    bool allow_surrogate = (mode == SWEEP_TEXT_NATIVE_SU);
    o.out = out; o.cap = out_cap; o.used = 0u; o.ok = true;
    if (!out && out_cap != 0u) return false;
    while (i < in_len) {
        size_t used = 0u;
        uint32_t cp = decode_one(in + i, in_len - i, &used, allow_surrogate);
        if (used == 0u) break;
        i += used;
        if (cp > 0xFFFFu) {
            if (mode == SWEEP_TEXT_WEB_COMPAT) {
                uint32_t v = cp - 0x10000u;
                uint32_t hi = 0xD800u + (v >> 10);
                uint32_t lo = 0xDC00u + (v & 0x3FFu);
                out_cp(&o, hi, true);
                out_cp(&o, lo, true);
            } else {
                out_cp(&o, SWEEP_CP_REPLACEMENT, false);
            }
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            if (mode == SWEEP_TEXT_WEB_COMPAT) out_cp(&o, SWEEP_CP_REPLACEMENT, false);
            else out_cp(&o, cp, true);
        } else {
            out_cp(&o, cp, false);
        }
    }
    if (out_len) *out_len = o.used;
    return o.ok;
}

size_t sweep_js_cesu8_decode(SWEEP_TEXT_MODE mode, const uint8_t* in,
                             size_t in_len, uint32_t* out, size_t cap,
                             size_t* out_count) {
    size_t i = 0u, n = 0u;
    if (out_count) *out_count = 0u;
    if (!in && in_len != 0u) return 0u;
    while (i < in_len) {
        size_t used = 0u;
        uint32_t cp = decode_one(in + i, in_len - i, &used, true);
        if (used == 0u) break;
        i += used;
        if (mode == SWEEP_TEXT_WEB_COMPAT && is_high(cp) && i < in_len) {
            size_t used2 = 0u;
            uint32_t lo = decode_one(in + i, in_len - i, &used2, true);
            if (is_low(lo)) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += used2;
            } else {
                cp = SWEEP_CP_REPLACEMENT;
            }
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            /* SuperUnicode has no surrogates (SUTR-0 §4.1, SUTR-1 §1.2):
             * 0xD800-0xDFFF are ordinary valid PUA codepoints and MUST be
             * accepted as values in byte decoding - never paired, never
             * rejected.  In WEB_COMPAT, a LONE surrogate-range value from the
             * web byte stream is unrepresentable at the web boundary, so it
             * becomes U+FFFD (a web-boundary sink, below-suite behavior);
             * NATIVE_SU keeps the value itself. */
            if (mode == SWEEP_TEXT_WEB_COMPAT) cp = SWEEP_CP_REPLACEMENT;
        }
        if (out && n < cap) out[n] = cp;
        ++n;
    }
    if (out_count) *out_count = n;
    return n;
}
