/* sweep_text.c - Sweep text modes / SUCS-web compatibility boundary.
 * See sweep_text.h for the 0xD800-0xDFFF invariant. */
#include "sweep_text.h"

#include <string.h>

bool sweep_sucs_is_valid(sweep_cp_t cp) {
    if (cp > SWEEP_CP_MAX) return false;
    if (cp >= SWEEP_CP_TRAP_MIN && cp <= SWEEP_CP_TRAP_MAX) return false;
    if (cp == SWEEP_CP_INVALID) return false;
    /* 0xD800-0xDFFF: valid SUCS values in native model. */
    return true;
}

static bool utf8_is_cont(uint8_t b) { return (b & 0xC0u) == 0x80u; }

static void emit_cp(sweep_cp_t cp, sweep_cp_t* out, size_t out_cap,
                    size_t* count) {
    if (*count < out_cap) out[*count] = cp;
    (*count)++;
}

size_t sweep_utf8_decode(SWEEP_TEXT_MODE mode, const uint8_t* in, size_t in_len,
                         sweep_cp_t* out, size_t out_cap, size_t* out_len) {
    size_t i = 0u;
    size_t n = 0u;
    (void)mode; /* byte decoding is shared; validity policy is per emitted cp */

    if (out_len) *out_len = 0u;
    if (!in && in_len) return 0u;

    while (i < in_len) {
        uint8_t b = in[i];
        sweep_cp_t cp = SWEEP_CP_REPLACEMENT;
        size_t seq = 1u;
        bool ok = false;

        if (b < 0x80u) {
            cp = b; ok = true;
        } else if ((b & 0xE0u) == 0xC0u) {
            seq = 2u;
            if (i + 1 < in_len && utf8_is_cont(in[i + 1])) {
                cp = ((sweep_cp_t)(b & 0x1Fu) << 6) | (sweep_cp_t)(in[i + 1] & 0x3Fu);
                ok = (cp >= 0x80u);
            }
        } else if ((b & 0xF0u) == 0xE0u) {
            seq = 3u;
            if (i + 2 < in_len && utf8_is_cont(in[i + 1]) && utf8_is_cont(in[i + 2])) {
                cp = ((sweep_cp_t)(b & 0x0Fu) << 12) |
                     ((sweep_cp_t)(in[i + 1] & 0x3Fu) << 6) |
                     (sweep_cp_t)(in[i + 2] & 0x3Fu);
                ok = (cp >= 0x800u);
                /* SuperUnicode has NO surrogates: 0xD800-0xDFFF are ordinary
                 * valid PUA codepoints (SUTR-0 §4.1, SUTR-1 §1.2/§4.2) and MUST
                 * be accepted in byte decoding.  They are simply never
                 * paired or combined (WEB_COMPAT pairs live only in the UTF-16
                 * boundary below).  The byte sequences ED A0 80 .. ED BF BF are
                 * therefore valid three-byte encodings, not errors.  Only
                 * WEB_COMPAT (strict web UTF-8) rejects them. */
                if (mode == SWEEP_TEXT_WEB_COMPAT &&
                    cp >= 0xD800u && cp <= 0xDFFFu) ok = false;
            }
        } else if ((b & 0xF8u) == 0xF0u) {
            seq = 4u;
            if (i + 3 < in_len && utf8_is_cont(in[i + 1]) && utf8_is_cont(in[i + 2]) &&
                utf8_is_cont(in[i + 3])) {
                cp = ((sweep_cp_t)(b & 0x07u) << 18) |
                     ((sweep_cp_t)(in[i + 1] & 0x3Fu) << 12) |
                     ((sweep_cp_t)(in[i + 2] & 0x3Fu) << 6) |
                     (sweep_cp_t)(in[i + 3] & 0x3Fu);
                ok = (cp >= 0x10000u && cp <= 0x10FFFFu);
            }
        } else {
            ok = false; seq = 1u;
        }

        if (!ok) { cp = SWEEP_CP_REPLACEMENT; seq = 1u; }
        emit_cp(cp, out, out_cap, &n);
        i += seq;
    }
    if (out_len) *out_len = n;
    return n;
}

static size_t encode_one_utf8(sweep_cp_t cp, uint8_t* out, size_t cap, size_t pos) {
    /* No 0xD800-0xDFFF rule here: those are valid SUCS PUA codepoints and
     * encode as ED A0 80..ED BF BF.  Mode-specific sinking (WEB boundary)
     * happens in sweep_utf8_encode before this call. */
    if (cp > 0x10FFFFu) cp = SWEEP_CP_REPLACEMENT;
    if (cp < 0x80u) {
        if (pos < cap) out[pos] = (uint8_t)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        if (pos + 1u < cap) {
            out[pos] = (uint8_t)(0xC0u | (cp >> 6));
            out[pos + 1u] = (uint8_t)(0x80u | (cp & 0x3Fu));
        }
        return 2u;
    }
    if (cp < 0x10000u) {
        if (pos + 2u < cap) {
            out[pos] = (uint8_t)(0xE0u | (cp >> 12));
            out[pos + 1u] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
            out[pos + 2u] = (uint8_t)(0x80u | (cp & 0x3Fu));
        }
        return 3u;
    }
    if (pos + 3u < cap) {
        out[pos] = (uint8_t)(0xF0u | (cp >> 18));
        out[pos + 1u] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        out[pos + 2u] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out[pos + 3u] = (uint8_t)(0x80u | (cp & 0x3Fu));
    }
    return 4u;
}

size_t sweep_utf8_encode(SWEEP_TEXT_MODE mode, const sweep_cp_t* in, size_t in_len,
                         uint8_t* out, size_t out_cap, size_t* out_len) {
    size_t i, pos = 0u;
    if (out_len) *out_len = 0u;
    if (!in && in_len) return 0u;
    for (i = 0u; i < in_len; ++i) {
        sweep_cp_t cp = in[i];
        if (mode == SWEEP_TEXT_NATIVE_SU) {
            /* NATIVE_SU: every valid SUCS codepoint up to 0x10FFFF encodes
             * as its SUTF-8/UTF-8 bridge form - INCLUDING 0xD800-0xDFFF.
             * They are valid PUA codepoints (SUTR-0 §4.1); encoding them as
             * ED A0 80..ED BF BF is correct and round-trips exactly, with
             * no pairing.  Only trap slots/sentinel (not sucs_is_valid) and
             * values above 0x10FFFF (beyond this helper's bridge range;
             * SUTF-8 5/6-byte forms are the native path) go to U+FFFD. */
            if (cp > 0x10FFFFu || !sweep_sucs_is_valid(cp)) cp = SWEEP_CP_REPLACEMENT;
        } else {
            /* WEB_COMPAT: strict web UTF-8 output - 0xD800-0xDFFF are not
             * representable in web UTF-8, so the web boundary
             * deterministically emits U+FFFD (this is a web-encoding sink,
             * not SUCS validity: those addresses stay valid codepoints). */
            if (cp >= 0xD800u && cp <= 0xDFFFu) cp = SWEEP_CP_REPLACEMENT;
        }
        pos += encode_one_utf8(cp, out, out_cap, pos);
    }
    if (out_len) *out_len = pos;
    return pos;
}

size_t sweep_utf16_decode(SWEEP_TEXT_MODE mode, const uint16_t* in, size_t in_len,
                          sweep_cp_t* out, size_t out_cap, size_t* out_len) {
    size_t i = 0u, n = 0u;
    if (out_len) *out_len = 0u;
    if (!in && in_len) return 0u;
    while (i < in_len) {
        uint16_t u = in[i++];
        if (mode == SWEEP_TEXT_WEB_COMPAT) {
            if (u >= 0xD800u && u <= 0xDBFFu) {
                if (i < in_len && in[i] >= 0xDC00u && in[i] <= 0xDFFFu) {
                    uint16_t lo = in[i++];
                    sweep_cp_t cp = 0x10000u + (((sweep_cp_t)(u - 0xD800u) << 10) | (sweep_cp_t)(lo - 0xDC00u));
                    emit_cp(cp, out, out_cap, &n);
                } else {
                    emit_cp(SWEEP_CP_REPLACEMENT, out, out_cap, &n);
                }
            } else if (u >= 0xDC00u && u <= 0xDFFFu) {
                emit_cp(SWEEP_CP_REPLACEMENT, out, out_cap, &n);
            } else {
                emit_cp((sweep_cp_t)u, out, out_cap, &n);
            }
        } else {
            /* NATIVE_SU: no surrogate pairing. Each 16-bit value is a code unit
             * value preserved as SUCS (0xD800-0xDFFF remain valid). */
            emit_cp((sweep_cp_t)u, out, out_cap, &n);
        }
    }
    if (out_len) *out_len = n;
    return n;
}

size_t sweep_utf16_encode(SWEEP_TEXT_MODE mode, const sweep_cp_t* in, size_t in_len,
                          uint16_t* out, size_t out_cap, size_t* out_len) {
    size_t i, n = 0u;
    if (out_len) *out_len = 0u;
    if (!in && in_len) return 0u;
    for (i = 0u; i < in_len; ++i) {
        sweep_cp_t cp = in[i];
        if (mode == SWEEP_TEXT_WEB_COMPAT) {
            if (cp >= 0xD800u && cp <= 0xDFFFu) cp = SWEEP_CP_REPLACEMENT;
            if (cp > 0x10FFFFu) cp = SWEEP_CP_REPLACEMENT;
            if (cp >= 0x10000u) {
                sweep_cp_t v = cp - 0x10000u;
                if (n < out_cap) out[n] = (uint16_t)(0xD800u | (v >> 10));
                n++;
                if (n < out_cap) out[n] = (uint16_t)(0xDC00u | (v & 0x3FFu));
                n++;
            } else {
                if (n < out_cap) out[n] = (uint16_t)cp;
                n++;
            }
        } else {
            /* NATIVE_SU: preserve 0xD800-0xDFFF as-is; no surrogate generation. */
            if (!sweep_sucs_is_valid(cp)) cp = SWEEP_CP_REPLACEMENT;
            if (cp > 0xFFFFu) {
                if (n < out_cap) out[n] = (uint16_t)SWEEP_CP_REPLACEMENT;
                n++;
            } else {
                if (n < out_cap) out[n] = (uint16_t)cp;
                n++;
            }
        }
    }
    if (out_len) *out_len = n;
    return n;
}

static bool mem_has_token(const uint8_t* data, size_t len, const char* tok) {
    size_t tlen = strlen(tok);
    size_t i;
    if (!data || !tok || tlen == 0u || len < tlen) return false;
    for (i = 0u; i + tlen <= len; ++i) {
        if (memcmp(data + i, tok, tlen) == 0) return true;
    }
    return false;
}

SWEEP_DOC_ENCODING sweep_detect_document_encoding(const uint8_t* data, size_t len,
                                                  const char* content_type) {
    /* Deterministic, explicit only. Non-ASCII/normal Unicode does NOT trigger. */
    if (content_type) {
        if (strstr(content_type, "text/x-superunicode") ||
            strstr(content_type, "application/x-superunicode") ||
            strstr(content_type, "sucs=native") ||
            strstr(content_type, "charset=superunicode")) {
            return SWEEP_DOC_ENC_NATIVE_SU;
        }
        if (strstr(content_type, "text/html") || strstr(content_type, "text/plain") ||
            strstr(content_type, "application/xhtml") || strstr(content_type, "charset=")) {
            return SWEEP_DOC_ENC_UNICODE_WEB;
        }
    }
    if (data && len >= 4u) {
        /* Explicit native marker only (not a Unicode BOM heuristic). */
        if (mem_has_token(data, len < 512u ? len : 512u, "<meta name=\"superunicode\" content=\"native\"")) {
            return SWEEP_DOC_ENC_NATIVE_SU;
        }
        if (data[0] == 0x00u && data[1] == 'S' && data[2] == 'U' && data[3] == 'N') {
            return SWEEP_DOC_ENC_NATIVE_SU;
        }
    }
    return SWEEP_DOC_ENC_UNKNOWN;
}
