/* sweep_text_test.c - host tests for Sweep text modes and the SUCS invariant.
 * Build (from Sweep root):
 *   gcc -std=c99 -Wall -Wextra -Werror -Isweep sweep/sweep_text.c sweep/tests/sweep_text_test.c -o build-generated/sweep_text_test.exe && build-generated/sweep_text_test.exe
 */
#include <stdio.h>
#include <string.h>

#include "../sweep_text.h"

static int g_failures = 0, g_passed = 0;
static void check(const char* label, int ok) {
    if (ok) { ++g_passed; printf("[PASS] %s\n", label); }
    else { ++g_failures; printf("[FAIL] %s\n", label); }
}

int main(void) {
    /* SUCS invariant: 0xD800-0xDFFF are VALID in native SuperUnicode. */
    check("sucs valid: U+0041", sweep_sucs_is_valid(0x0041u));
    check("sucs valid: U+D800 (native PUA-like SUCS, NOT a surrogate)",
          sweep_sucs_is_valid(0xD800u));
    check("sucs valid: U+DFFF (native)", sweep_sucs_is_valid(0xDFFFu));
    check("sucs valid: U+10FFFF", sweep_sucs_is_valid(0x10FFFFu));
    check("sucs invalid: trap range", !sweep_sucs_is_valid(0x7FFFFFF0u));
    check("sucs invalid: sentinel", !sweep_sucs_is_valid(0x7FFFFFFFu));

    /* WEB_COMPAT UTF-16: surrogate pair decoded, unpaired -> replacement. */
    {
        uint16_t in[] = { 0x0041u, 0xD83Du, 0xDE00u, 0xD800u }; /* A, U+1F600, lone high */
        sweep_cp_t out[8]; size_t n = 0;
        sweep_utf16_decode(SWEEP_TEXT_WEB_COMPAT, in, 4u, out, 8u, &n);
        check("web utf16: count 3", n == 3u);
        check("web utf16: A", out[0] == 0x0041u);
        check("web utf16: surrogate pair -> U+1F600", out[1] == 0x1F600u);
        check("web utf16: lone surrogate -> replacement", out[2] == SWEEP_CP_REPLACEMENT);
    }
    /* NATIVE_SU UTF-16 code-unit mapping: NO surrogate pairing; 0xD800 preserved. */
    {
        uint16_t in[] = { 0xD83Du, 0xDE00u, 0xD800u };
        sweep_cp_t out[8]; size_t n = 0;
        sweep_utf16_decode(SWEEP_TEXT_NATIVE_SU, in, 3u, out, 8u, &n);
        check("native utf16: no pairing, count 3", n == 3u);
        check("native utf16: preserves 0xD83D as SUCS value", out[0] == 0xD83Du);
        check("native utf16: preserves 0xDE00 as SUCS value", out[1] == 0xDE00u);
        check("native utf16: preserves 0xD800 as valid SUCS", out[2] == 0xD800u);
    }
    /* WEB_COMPAT encode: >BMP becomes surrogate pair; 0xD800 scalar -> replacement. */
    {
        sweep_cp_t in[] = { 0x1F600u, 0xD800u };
        uint16_t out[8]; size_t n = 0;
        sweep_utf16_encode(SWEEP_TEXT_WEB_COMPAT, in, 2u, out, 8u, &n);
        check("web utf16 enc: pair count 3", n == 3u);
        check("web utf16 enc: high", out[0] == 0xD83Du);
        check("web utf16 enc: low", out[1] == 0xDE00u);
        check("web utf16 enc: D800 scalar -> replacement", out[2] == 0xFFFDu);
    }
    /* NATIVE_SU encode: 0xD800 emitted as-is (valid SUCS), no surrogate generation. */
    {
        sweep_cp_t in[] = { 0xD800u, 0x0042u };
        uint16_t out[8]; size_t n = 0;
        sweep_utf16_encode(SWEEP_TEXT_NATIVE_SU, in, 2u, out, 8u, &n);
        check("native utf16 enc: preserves D800", n == 2u && out[0] == 0xD800u);
        check("native utf16 enc: B", out[1] == 0x0042u);
    }
    /* UTF-8 decode: valid, multibyte, malformed recovery. */
    {
        const uint8_t in[] = { 'A', 0xC2u, 0xA9u, 0xE2u, 0x82u, 0xACu, 0xFFu };
        sweep_cp_t out[8]; size_t n = 0;
        sweep_utf8_decode(SWEEP_TEXT_WEB_COMPAT, in, sizeof(in), out, 8u, &n);
        check("utf8: count 4 (A, copyright, euro, replacement)", n == 4u);
        check("utf8: A", out[0] == 'A');
        check("utf8: U+00A9", out[1] == 0x00A9u);
        check("utf8: U+20AC", out[2] == 0x20ACu);
        check("utf8: malformed -> replacement", out[3] == SWEEP_CP_REPLACEMENT);
    }
    /* UTF-8 decode rejects surrogate-encoded UTF-8 bytes (malformed on web). */
    {
        const uint8_t in[] = { 0xEDu, 0xA0u, 0x80u }; /* would encode U+D800 in CESU-8/UTF-8 form - malformed */
        sweep_cp_t out[4]; size_t n = 0;
        sweep_utf8_decode(SWEEP_TEXT_WEB_COMPAT, in, sizeof(in), out, 4u, &n);
        check("utf8: surrogate-encoded bytes rejected", n >= 1u && out[0] == SWEEP_CP_REPLACEMENT);
    }
    /* NATIVE_SU: the entire 0xD800-0xDFFF block round-trips exactly
     * (SUTR-0 §4.1: valid PUA, never rejected; SUTR-1 §1.2/§4.2: no pairing). */
    {
        uint32_t cp;
        int ok = 1;
        for (cp = 0xD800u; cp <= 0xDFFFu; ++cp) {
            sweep_cp_t in[1] = { (sweep_cp_t) cp };
            uint8_t bytes[4];
            size_t elen = 0u, dlen = 0u;
            sweep_cp_t out[2];
            sweep_utf8_encode(SWEEP_TEXT_NATIVE_SU, in, 1u, bytes, sizeof(bytes), &elen);
            sweep_utf8_decode(SWEEP_TEXT_NATIVE_SU, bytes, elen, out, 2u, &dlen);
            if (elen != 3u || dlen != 1u || out[0] != (sweep_cp_t) cp ||
                bytes[0] != 0xEDu) { ok = 0; break; }
        }
        check("native utf8: full D800-DFFF block encode/decode round-trip", ok);
        /* Spot-check the canonical bytes of both block edges + middle. */
        {
            sweep_cp_t in1[1] = { 0xD800u }, in2[1] = { 0xDBFFu }, in3[1] = { 0xDFFFu };
            uint8_t b[4]; size_t elen = 0u;
            sweep_utf8_encode(SWEEP_TEXT_NATIVE_SU, in1, 1u, b, sizeof(b), &elen);
            check("native utf8: D800 is ED A0 80", elen == 3u && b[0] == 0xEDu && b[1] == 0xA0u && b[2] == 0x80u);
            sweep_utf8_encode(SWEEP_TEXT_NATIVE_SU, in2, 1u, b, sizeof(b), &elen);
            check("native utf8: DBFF is ED AF BF", elen == 3u && b[0] == 0xEDu && b[1] == 0xAFu && b[2] == 0xBFu);
            sweep_utf8_encode(SWEEP_TEXT_NATIVE_SU, in3, 1u, b, sizeof(b), &elen);
            check("native utf8: DFFF is ED BF BF", elen == 3u && b[0] == 0xEDu && b[1] == 0xBFu && b[2] == 0xBFu);
        }
        /* Trap/sentinel still rejected upstream of encoding (not dropped data). */
        {
            sweep_cp_t traps[2] = { 0x7FFFFFF0u, 0x7FFFFFFFu };
            uint8_t b[8]; size_t elen = 0u;
            sweep_cp_t out[3]; size_t dlen = 0u;
            sweep_utf8_encode(SWEEP_TEXT_NATIVE_SU, traps, 2u, b, sizeof(b), &elen);
            sweep_utf8_decode(SWEEP_TEXT_NATIVE_SU, b, elen, out, 3u, &dlen);
            check("native utf8: trap/sentinel sink to replacement",
                  dlen == 2u && out[0] == SWEEP_CP_REPLACEMENT && out[1] == SWEEP_CP_REPLACEMENT);
        }
        /* A pair of adjacent block values is TWO values, never one pair. */
        {
            const uint8_t bytes[] = { 0xEDu, 0xA0u, 0xBDu, 0xEDu, 0xB8u, 0x80u };
            sweep_cp_t out[3]; size_t dlen = 0u;
            sweep_utf8_decode(SWEEP_TEXT_NATIVE_SU, bytes, sizeof(bytes), out, 3u, &dlen);
            check("native utf8: adjacent block values never pair",
                  dlen == 2u && out[0] == 0xD83Du && out[1] == 0xDE00u);
        }
        /* Valid neighbours of the block are unaffected (no over-rejection). */
        {
            const uint8_t bytes[] = { 0xEEu, 0x80u, 0x80u, 0xF0u, 0x9Fu, 0x98u, 0x80u };
            sweep_cp_t out[3]; size_t dlen = 0u;
            sweep_utf8_decode(SWEEP_TEXT_NATIVE_SU, bytes, sizeof(bytes), out, 3u, &dlen);
            check("native utf8: PUA + astral neighbours decode",
                  dlen == 2u && out[0] == 0xE000u && out[1] == 0x1F600u);
        }
    }
    /* Document detection: explicit only. Normal Unicode never triggers native. */
    {
        const char html[] = "<html><body>Hello \xC3\xA9</body></html>";
        check("detect: normal html is unknown/web", 
              sweep_detect_document_encoding((const uint8_t*)html, strlen(html), "text/html") == SWEEP_DOC_ENC_UNICODE_WEB);
        check("detect: explicit native content-type",
              sweep_detect_document_encoding((const uint8_t*)"", 0, "text/x-superunicode; sucs=native") == SWEEP_DOC_ENC_NATIVE_SU);
        check("detect: explicit meta marker",
              sweep_detect_document_encoding((const uint8_t*)"<meta name=\"superunicode\" content=\"native\">", 44u, 0) == SWEEP_DOC_ENC_NATIVE_SU);
        check("detect: normal unicode content no native trigger",
              sweep_detect_document_encoding((const uint8_t*)"\xED\xA0\x80", 3u, "text/html") != SWEEP_DOC_ENC_NATIVE_SU);
    }

    printf("=== Sweep text: %d passed, %d failed ===\n", g_passed, g_failures);
    return g_failures ? 1 : 0;
}
