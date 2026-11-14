/* sweep_services.h - Sweep browser-engine services (KDB Service Bridge contract).
 *
 * Service handlers follow the OW_KDB_SERVICE_HANDLER contract: requests and
 * responses are KDB-owned copies, lengths are validated by KDB, and handlers
 * must not retain pointers.  The handlers themselves are plain functions so
 * they can be unit-tested without the full kernel.
 *
 * Service ID range (owner = Sweep instance): 0x53570000 + n ("SW" prefix).
 */
#ifndef SWEEP_SERVICES_H
#define SWEEP_SERVICES_H

#include <stdint.h>

#include "../inc/ow_types.h"

#define SWEEP_SVC_URL_ADMIT      0x53570001U
#define SWEEP_SVC_VIEWPORT       0x53570002U
#define SWEEP_SVC_PAINT_APPEND   0x53570003U

/* Request: NUL-terminated or raw URL bytes (length = byte count).
 * Response on success: 1 byte admitted flag (1/0) + host bytes + NUL. */
OW_STATUS SweepSvcUrlAdmit(uint32_t ServiceId,
                           const uint8_t* Request, uint32_t RequestLength,
                           uint8_t* Response, uint32_t ResponseCapacity,
                           uint32_t* ResponseLength, void* Context);

/* Request: width_px, height_px, device_scale as three uint32_t (little-endian,
 * 12 bytes).  Response: negotiated width, height, scale as three uint32_t. */
OW_STATUS SweepSvcViewport(uint32_t ServiceId,
                           const uint8_t* Request, uint32_t RequestLength,
                           uint8_t* Response, uint32_t ResponseCapacity,
                           uint32_t* ResponseLength, void* Context);

/* Request: one SWEEP_PAINT_CMD wire form (op,x,y,w,h,color,glyph = 29 bytes).
 * Response: uint32_t accepted (1/0).  Context points to a
 * SWEEP_PAINT_SESSION holding the bounded list. */
typedef struct _SWEEP_PAINT_CMD SWEEP_PAINT_CMD;
typedef struct _SWEEP_PAINT_SESSION {
    SWEEP_PAINT_CMD* list;
    uint32_t         capacity;
    uint32_t         count;
} SWEEP_PAINT_SESSION;

OW_STATUS SweepSvcPaintAppend(uint32_t ServiceId,
                              const uint8_t* Request, uint32_t RequestLength,
                              uint8_t* Response, uint32_t ResponseCapacity,
                              uint32_t* ResponseLength, void* Context);

/* Register all Sweep services owned by OwnerInstanceId.  Requires KDB to be
 * ready and OwnerInstanceId to be a live instance (or 0 is refused here;
 * Sweep services belong to the Sweep associated-kernel instance).
 * Implemented in sweep_kdb_register.c (links against KDB). */
OW_STATUS SweepRegisterKdbServices(uint32_t OwnerInstanceId,
                                   SWEEP_PAINT_SESSION* PaintSession);

#endif /* SWEEP_SERVICES_H */
