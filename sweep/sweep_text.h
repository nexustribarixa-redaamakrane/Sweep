/* sweep_text.h - Sweep text modes and SUCS/Unicode compatibility boundary.
 *
 * SuperUnicode invariant: 0xD800-0xDFFF are ordinary valid SUCS codepoints in
 * NATIVE_SU mode (PUA values).  Surrogate-pair interpretation is applied ONLY
 * at the WEB_COMPAT UTF-16 boundary when processing conventional web content.
 * Never combine adjacent native SUCS values as surrogate halves.
 */
#ifndef SWEEP_TEXT_H
#define SWEEP_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t sweep_cp_t; /* SUCS/Unicode scalar value container */

#define SWEEP_CP_INVALID      0x7FFFFFFFu
#define SWEEP_CP_MAX          0x7FFFFFFFu
#define SWEEP_CP_TRAP_MIN     0x7FFFFFF0u
#define SWEEP_CP_TRAP_MAX     0x7FFFFFFEu
#define SWEEP_CP_REPLACEMENT  0xFFFDu

typedef enum _SWEEP_TEXT_MODE {
    SWEEP_TEXT_WEB_COMPAT = 0, /* ordinary Unicode/web semantics at boundaries */
    SWEEP_TEXT_NATIVE_SU  = 1  /* native SUCS preserved end-to-end */
} SWEEP_TEXT_MODE;

/* SUCS validity for native processing: 0xD800-0xDFFF are VALID (not surrogates).
 * Trap range and invalid sentinel are rejected. */
bool sweep_sucs_is_valid(sweep_cp_t cp);

/* UTF-8 (web byte stream) -> SUCS codepoints.
 * WEB_COMPAT: malformed sequences emit U+FFFD and consume one byte (deterministic
 * recovery). Overlong/surrogate-encoded UTF-8 is treated as malformed.
 * NATIVE_SU: SuperUnicode has no surrogates, so the range 0xD800-0xDFFF is
 * 2,048 ordinary valid PUA codepoints (SUTR-0 §4.1, SUTR-1 §1.2/§4.2).
 * Every value in that range MUST be accepted and decoded as itself -
 * never paired, never combined, never rejected - so ED A0 80..ED BF BF are
 * valid three-byte encodings that round-trip exactly.  Trap slots
 * (0x7FFFFFF0-0x7FFFFFFE) and the sentinel (0x7FFFFFFF) stay rejected via
 * sweep_sucs_is_valid; this helper covers the 1-4-byte Unicode bridge
 * (SUTF-8 5/6-byte forms above 0x10FFFF belong to the SUTF APIs).
 * Returns number of codepoints written (<= out_cap). Sets *out_len. */
size_t sweep_utf8_decode(SWEEP_TEXT_MODE mode, const uint8_t* in, size_t in_len,
                         sweep_cp_t* out, size_t out_cap, size_t* out_len);

/* SUCS codepoints -> UTF-8 bytes.
 * In NATIVE_SU, 0xD800-0xDFFF encode as their three-byte SUTF-8 forms
 * (ED A0 80..ED BF BF) and round-trip exactly; they are valid PUA
 * codepoints with no surrogate semantics.  Only trap/sentinel values and
 * values above 0x10FFFF (beyond this helper's bridge range) go to U+FFFD.
 * In WEB_COMPAT, codepoints in 0xD800-0xDFFF are not valid Unicode scalars and
 * are encoded as U+FFFD replacement (web boundary safety). */
size_t sweep_utf8_encode(SWEEP_TEXT_MODE mode, const sweep_cp_t* in, size_t in_len,
                         uint8_t* out, size_t out_cap, size_t* out_len);

/* UTF-16 code units (host-endian) -> SUCS codepoints.
 * WEB_COMPAT ONLY path that interprets surrogate pairs: a high surrogate
 * followed by a low surrogate becomes one Unicode scalar; unpaired surrogates
 * become U+FFFD. This must not be used to reinterpret native SUCS values.
 * In NATIVE_SU, each uint16_t is treated as a code unit value mapped directly
 * to a SUCS codepoint (no surrogate pairing) - useful only for explicit native
 * 16-bit transports; native SUTF-16 is the authoritative transform. */
size_t sweep_utf16_decode(SWEEP_TEXT_MODE mode, const uint16_t* in, size_t in_len,
                          sweep_cp_t* out, size_t out_cap, size_t* out_len);

/* SUCS codepoints -> UTF-16 code units (host-endian).
 * WEB_COMPAT: scalars > U+FFFF become surrogate pairs; 0xD800-0xDFFF (invalid
 * Unicode scalars on the web boundary) become U+FFFD.
 * NATIVE_SU: no surrogate generation; values > 0xFFFF are not representable as
 * single UTF-16 compatibility code units and are emitted as U+FFFD (caller
 * should use native SUTF/SUCS paths). 0xD800-0xDFFF are emitted as-is (valid
 * native SUCS values). */
size_t sweep_utf16_encode(SWEEP_TEXT_MODE mode, const sweep_cp_t* in, size_t in_len,
                          uint16_t* out, size_t out_cap, size_t* out_len);

/* Native SuperUnicode document detection (deterministic, explicit only).
 * Returns true only for explicit markers. Normal Unicode/non-ASCII content does
 * NOT trigger native mode. */
typedef enum _SWEEP_DOC_ENCODING {
    SWEEP_DOC_ENC_UNKNOWN = 0,
    SWEEP_DOC_ENC_UNICODE_WEB,
    SWEEP_DOC_ENC_NATIVE_SU /* explicit native SuperUnicode marker */
} SWEEP_DOC_ENCODING;

SWEEP_DOC_ENCODING sweep_detect_document_encoding(const uint8_t* data, size_t len,
                                                  const char* content_type);

#endif /* SWEEP_TEXT_H */
