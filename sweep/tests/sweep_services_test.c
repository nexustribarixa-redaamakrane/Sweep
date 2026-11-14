/* sweep_services_test.c - host test for Sweep KDB service handlers (contract).
 * Build (from Sweep root):
 *   gcc -std=c99 -Wall -Wextra -Werror -Isweep -Iinc
 *       sweep/sweep_engine.c sweep/sweep_services.c sweep/tests/sweep_services_test.c
 *       -o build-generated/sweep_services_test && build-generated/sweep_services_test
 */
#include <stdio.h>
#include <string.h>

#include "../sweep_services.h"
#include "../sweep_engine.h"

static int g_failures = 0;
static int g_passed = 0;

static void check(const char* label, int ok) {
    if (ok) {
        ++g_passed;
        printf("[PASS] %s\n", label);
    } else {
        ++g_failures;
        printf("[FAIL] %s\n", label);
    }
}

static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void) {
    uint8_t response[256];
    uint32_t out_len = 0u;
    OW_STATUS st;

    /* URL admission: valid URL -> admitted + host */
    {
        const char* url = "https://example.org:443/index";
        st = SweepSvcUrlAdmit(SWEEP_SVC_URL_ADMIT, (const uint8_t*)url,
                              (uint32_t)strlen(url), response, sizeof(response),
                              &out_len, (void*)0);
        check("url admit returns success", st == OW_SUCCESS);
        check("url admit reports admitted", out_len >= 1u && response[0] == 1u);
        check("url admit echoes host",
              out_len > 2u && strcmp((const char*)(response + 1u), "example.org") == 0);
    }

    /* URL admission: invalid URL -> not admitted, still success status */
    {
        const char* url = "not-a-url";
        st = SweepSvcUrlAdmit(SWEEP_SVC_URL_ADMIT, (const uint8_t*)url,
                              (uint32_t)strlen(url), response, sizeof(response),
                              &out_len, (void*)0);
        check("bad url returns success", st == OW_SUCCESS);
        check("bad url not admitted", out_len >= 1u && response[0] == 0u);
    }

    /* URL admission: tiny response buffer -> admitted flag only */
    {
        const char* url = "http://example.com/";
        uint8_t tiny[2];
        st = SweepSvcUrlAdmit(SWEEP_SVC_URL_ADMIT, (const uint8_t*)url,
                              (uint32_t)strlen(url), tiny, sizeof(tiny),
                              &out_len, (void*)0);
        check("tiny buffer still succeeds", st == OW_SUCCESS);
        check("tiny buffer returns admitted flag", out_len == 1u && tiny[0] == 1u);
    }

    /* URL admission: insufficient capacity for even the flag */
    {
        const char* url = "http://example.com/";
        uint8_t none[1];
        st = SweepSvcUrlAdmit(SWEEP_SVC_URL_ADMIT, (const uint8_t*)url,
                              (uint32_t)strlen(url), none, 0u, &out_len, (void*)0);
        check("zero capacity refused", st == OW_ERR_INSUFFICIENT);
    }

    /* Viewport negotiation */
    {
        uint8_t req[12];
        wr_u32(req + 0u, 10000u);
        wr_u32(req + 4u, 5000u);
        wr_u32(req + 8u, 8u);
        st = SweepSvcViewport(SWEEP_SVC_VIEWPORT, req, 12u, response,
                              sizeof(response), &out_len, (void*)0);
        check("viewport returns success", st == OW_SUCCESS);
        check("viewport response length 12", out_len == 12u);
        check("viewport clamps width", rd_u32(response + 0u) == 8192u);
        check("viewport clamps height", rd_u32(response + 4u) == 5000u);
        check("viewport clamps scale", rd_u32(response + 8u) == 4u);
    }

    /* Viewport: wrong request length */
    {
        uint8_t req[8];
        memset(req, 0, sizeof(req));
        st = SweepSvcViewport(SWEEP_SVC_VIEWPORT, req, 8u, response,
                              sizeof(response), &out_len, (void*)0);
        check("viewport bad length refused", st == OW_ERR_INVALID_PARAM);
    }

    /* Paint append */
    {
        SWEEP_PAINT_CMD list[2];
        SWEEP_PAINT_SESSION session;
        uint8_t req[29];
        memset(list, 0, sizeof(list));
        session.list = list;
        session.capacity = 2u;
        session.count = 0u;

        memset(req, 0, sizeof(req));
        req[0] = (uint8_t)SWEEP_PAINT_CLEAR;
        st = SweepSvcPaintAppend(SWEEP_SVC_PAINT_APPEND, req, 29u, response,
                                 sizeof(response), &out_len, &session);
        check("paint clear accepted", st == OW_SUCCESS && rd_u32(response) == 1u);
        check("paint count advanced", session.count == 1u);

        memset(req, 0, sizeof(req));
        req[0] = (uint8_t)SWEEP_PAINT_TEXT;
        wr_u32(req + 21u, (uint32_t)'Z'); /* glyph */
        st = SweepSvcPaintAppend(SWEEP_SVC_PAINT_APPEND, req, 29u, response,
                                 sizeof(response), &out_len, &session);
        check("paint text accepted", st == OW_SUCCESS && rd_u32(response) == 1u);

        /* session full */
        st = SweepSvcPaintAppend(SWEEP_SVC_PAINT_APPEND, req, 29u, response,
                                 sizeof(response), &out_len, &session);
        check("paint full reports not accepted",
              st == OW_SUCCESS && rd_u32(response) == 0u);
    }

    printf("=== Sweep services: %d passed, %d failed ===\n", g_passed, g_failures);
    return g_failures != 0;
}
