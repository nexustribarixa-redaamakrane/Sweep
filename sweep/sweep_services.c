/* sweep_services.c - Sweep browser-engine KDB service handlers. */
#include "sweep_services.h"

#include <string.h>

#include "sweep_engine.h"

static uint32_t rd_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i32(const uint8_t* p) {
    uint32_t v = rd_u32(p);
    int32_t r;
    memcpy(&r, &v, sizeof(r));
    return r;
}

static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

OW_STATUS SweepSvcUrlAdmit(uint32_t ServiceId,
                           const uint8_t* Request, uint32_t RequestLength,
                           uint8_t* Response, uint32_t ResponseCapacity,
                           uint32_t* ResponseLength, void* Context) {
    char urlbuf[SWEEP_URL_MAX + 1];
    SWEEP_URL parsed;
    bool ok;
    uint32_t host_n;
    uint32_t needed;

    (void)ServiceId;
    (void)Context;

    if (ResponseCapacity < 2u || ResponseLength == (uint32_t*)0) {
        return OW_ERR_INSUFFICIENT;
    }
    if (RequestLength == 0u || RequestLength > SWEEP_URL_MAX) {
        Response[0] = 0u;
        *ResponseLength = 1u;
        return OW_SUCCESS;
    }
    memcpy(urlbuf, Request, RequestLength);
    urlbuf[RequestLength] = '\0';

    ok = SweepUrlParse(urlbuf, &parsed);
    Response[0] = ok ? 1u : 0u;
    host_n = ok ? (uint32_t)strlen(parsed.host) : 0u;
    needed = 1u + host_n + 1u;
    if (needed > ResponseCapacity) {
        *ResponseLength = 1u;
        return OW_SUCCESS;
    }
    if (ok) {
        memcpy(Response + 1u, parsed.host, host_n);
    }
    Response[1u + host_n] = 0u;
    *ResponseLength = needed;
    return OW_SUCCESS;
}

OW_STATUS SweepSvcViewport(uint32_t ServiceId,
                           const uint8_t* Request, uint32_t RequestLength,
                           uint8_t* Response, uint32_t ResponseCapacity,
                           uint32_t* ResponseLength, void* Context) {
    SWEEP_VIEWPORT req;
    SWEEP_VIEWPORT neg;

    (void)ServiceId;
    (void)Context;

    if (ResponseCapacity < 12u || ResponseLength == (uint32_t*)0) {
        return OW_ERR_INSUFFICIENT;
    }
    if (Request == (const uint8_t*)0 || RequestLength != 12u) {
        return OW_ERR_INVALID_PARAM;
    }
    req.width_px = rd_u32(Request + 0u);
    req.height_px = rd_u32(Request + 4u);
    req.device_scale = rd_u32(Request + 8u);

    if (!SweepViewportNegotiate(&req, &neg)) {
        return OW_ERR_INVALID_PARAM;
    }
    wr_u32(Response + 0u, neg.width_px);
    wr_u32(Response + 4u, neg.height_px);
    wr_u32(Response + 8u, neg.device_scale);
    *ResponseLength = 12u;
    return OW_SUCCESS;
}

OW_STATUS SweepSvcPaintAppend(uint32_t ServiceId,
                              const uint8_t* Request, uint32_t RequestLength,
                              uint8_t* Response, uint32_t ResponseCapacity,
                              uint32_t* ResponseLength, void* Context) {
    SWEEP_PAINT_SESSION* session = (SWEEP_PAINT_SESSION*)Context;
    SWEEP_PAINT_CMD cmd;
    size_t count;
    bool accepted;

    (void)ServiceId;

    if (ResponseCapacity < 4u || ResponseLength == (uint32_t*)0) {
        return OW_ERR_INSUFFICIENT;
    }
    if (session == (SWEEP_PAINT_SESSION*)0 || session->list == (SWEEP_PAINT_CMD*)0) {
        return OW_ERR_INVALID_PARAM;
    }
    if (Request == (const uint8_t*)0 || RequestLength != 29u) {
        return OW_ERR_INVALID_PARAM;
    }

    cmd.op = Request[0];
    cmd.x = rd_i32(Request + 1u);
    cmd.y = rd_i32(Request + 5u);
    cmd.w = rd_u32(Request + 9u);
    cmd.h = rd_u32(Request + 13u);
    cmd.color = rd_u32(Request + 17u);
    cmd.glyph = rd_u32(Request + 21u);
    /* bytes 25..28 reserved */

    count = (size_t)session->count;
    accepted = SweepPaintAppend(session->list, (size_t)session->capacity,
                                &count, cmd);
    session->count = (uint32_t)count;
    wr_u32(Response, accepted ? 1u : 0u);
    *ResponseLength = 4u;
    return OW_SUCCESS;
}

/* SweepRegisterKdbServices lives in sweep_kdb_register.c so the handlers in
 * this file stay linkable in standalone host tests without the KDB runtime. */
