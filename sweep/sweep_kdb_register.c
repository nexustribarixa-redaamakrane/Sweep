/* sweep_kdb_register.c - registers Sweep browser-engine services with KDB.
 * Split from sweep_services.c so the handlers remain host-testable without
 * linking the full KDB runtime.
 */
#include "sweep_services.h"

#include "../inc/ow_kdb.h"

OW_STATUS SweepRegisterKdbServices(uint32_t OwnerInstanceId,
                                   SWEEP_PAINT_SESSION* PaintSession) {
    OW_STATUS status;

    if (OwnerInstanceId == OW_KDB_INVALID_ID) {
        return OW_ERR_INVALID_PARAM;
    }

    status = OwKdbServiceRegister(OwnerInstanceId, SWEEP_SVC_URL_ADMIT,
                                  "sweep.url.admit", OW_KDB_CAP_CALL,
                                  256u, 256u, SweepSvcUrlAdmit, (void*)0);
    if (ow_status_error(status)) {
        return status;
    }
    status = OwKdbServiceRegister(OwnerInstanceId, SWEEP_SVC_VIEWPORT,
                                  "sweep.viewport", OW_KDB_CAP_CALL,
                                  12u, 12u, SweepSvcViewport, (void*)0);
    if (ow_status_error(status)) {
        return status;
    }
    status = OwKdbServiceRegister(OwnerInstanceId, SWEEP_SVC_PAINT_APPEND,
                                  "sweep.paint.append", OW_KDB_CAP_CALL,
                                  29u, 4u, SweepSvcPaintAppend,
                                  (void*)PaintSession);
    return status;
}
