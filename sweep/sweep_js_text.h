/* sweep_js_text.h - explicit engine-string/text-domain boundary.
 *
 * Duktape stores strings in an extended UTF-8 form (CESU-8 for non-BMP
 * characters: a surrogate pair encoded as two 3-byte sequences).  Sweep's DOM
 * stores web text as UTF-8 bytes.  These helpers make the conversion explicit
 * and mode-dependent instead of silently reinterpreting bytes.
 *
 * WEB_COMPAT follows ECMAScript/UTF-16 code-unit semantics: surrogate pairs
 * become one Unicode scalar, and unpaired surrogates become U+FFFD at the DOM
 * boundary.
 *
 * NATIVE_SU treats every 16-bit value, including 0xD800-0xDFFF, as an ordinary
 * SUCS private-use value.  It never pairs, rejects or reinterprets those
 * values.  Values above U+FFFF are not representable as one 16-bit engine
 * character; current helpers replace them (documented limitation, not a
 * surrogate conversion).
 */
#ifndef SWEEP_JS_TEXT_H
#define SWEEP_JS_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sweep_text.h"

/* Extended UTF-8/CESU-8 (engine form) -> web UTF-8 (WEB_COMPAT) or native
 * CESU-8-preserving bytes (NATIVE_SU).  Returns true when the whole input was
 * converted; false if the output buffer was too small (out_len still reports
 * how much was written). */
bool sweep_js_cesu8_to_utf8(SWEEP_TEXT_MODE mode, const uint8_t* in,
                            size_t in_len, uint8_t* out, size_t out_cap,
                            size_t* out_len);

/* Web UTF-8 -> extended UTF-8/CESU-8 (engine form).  Same return convention. */
bool sweep_js_utf8_to_cesu8(SWEEP_TEXT_MODE mode, const uint8_t* in,
                            size_t in_len, uint8_t* out, size_t out_cap,
                            size_t* out_len);

/* Extended UTF-8/CESU-8 byte stream -> SUCS codepoints (transport helper).
 * SuperUnicode has no surrogates (SUTR-0 §4.1, SUTR-1 §1.2): the range
 * 0xD800-0xDFFF is 2,048 ordinary valid PUA codepoints and is NEVER
 * paired, combined, or rejected here.  NATIVE_SU decodes every value in
 * that range as itself, so the whole block round-trips exactly.  Valid
 * sequences are unaffected: ASCII, PUA (0xE000+), and four-byte astral
 * scalars decode normally in both modes.  WEB_COMPAT additionally keeps
 * ECMAScript code-unit semantics at the web boundary (pairs combine;
 * a lone surrogate-range value from the web byte stream sinks to U+FFFD),
 * and values above U+FFFF stay one value (the 16-bit engine-string layer
 * is the lossier one, see sweep_js_utf8_to_cesu8).
 * Writes up to cap codepoints; returns the total count produced (the caller
 * can detect overflow as count > cap, mirroring the other helpers). */
size_t sweep_js_cesu8_decode(SWEEP_TEXT_MODE mode, const uint8_t* in,
                             size_t in_len, uint32_t* out, size_t cap,
                             size_t* out_count);

#endif /* SWEEP_JS_TEXT_H */
