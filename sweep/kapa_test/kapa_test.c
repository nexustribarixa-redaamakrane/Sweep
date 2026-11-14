/* kapa_test.c - minimal controlled KAPA payload for the R5 sweep-kernel profile.
 *
 * The first genuine KAPA execution path needs a first payload that is tiny,
 * controlled and honest.  This one does not touch any Sweep browser engine, any
 * filesystem, any network and any compositor: it builds one Service Bridge
 * descriptor, makes one bounded main-kernel service call (main.log), makes one
 * main-kernel service call (main.ping) and then exits with a defined status.
 *
 * Everything it touches lives in its own image/stack VAD (static message in a
 * read-only image section, response buffer on the user stack), so the kernel
 * gateway's residency checks have real pages to validate.  It uses no libc, no
 * heap, no imports and no host facilities: freestanding, static/stack only.
 *
 * Exit status is defined (0x5A) so the KDB instance lifecycle can be observed
 * end to end: exit -> OwKdbNotifyThreadExit -> OwKdbManagerFinish -> STOPPED.
 */
#include "../emergency/ow_gate.h"

void* memcpy(void* dst, const void* src, unsigned long n);
void* memset(void* dst, int c, unsigned long n);

int kapa_test_main(void);

static const char k_kapa_message[] = "kapa-r5-entry-ok";

int kapa_test_main(void) {
    OW_SYS_KDB_BRIDGE_CALL call;
    uint8_t response[16];
    int64_t rc;

    /* main.log: bounded attributed message, no response payload. */
    call.Magic = OW_SYS_KDB_BRIDGE_MAGIC;
    call.Version = OW_SYS_KDB_BRIDGE_VERSION;
    call.ServiceId = OW_KDB_SERVICE_LOG;
    call.Flags = 0U;
    call.RequestLength = (uint32_t)(sizeof(k_kapa_message) - 1u);
    call.ResponseCapacity = 0U;
    call.ResponseLength = 0U;
    call.TargetInstanceId = 0U;
    call.RequestAddress = (uint64_t)(uintptr_t)k_kapa_message;
    call.ResponseAddress = 0U;
    call.Reserved = 0U;

    rc = ow_gate_bridge(&call);
    if (rc != 0) {
        /* Defined failure path: report and exit with a distinct status. */
        return 3;
    }

    /* main.ping: requestless call, 8-byte response, read back through the
     * gateway's residency-checked copy-out. */
    call.Magic = OW_SYS_KDB_BRIDGE_MAGIC;
    call.Version = OW_SYS_KDB_BRIDGE_VERSION;
    call.ServiceId = OW_KDB_SERVICE_PING;
    call.Flags = 0U;
    call.RequestLength = 0U;
    call.ResponseCapacity = 16U;
    call.ResponseLength = 0U;
    call.TargetInstanceId = 0U;
    call.RequestAddress = 0U;
    call.ResponseAddress = (uint64_t)(uintptr_t)response;
    call.Reserved = 0U;

    rc = ow_gate_bridge(&call);
    if (rc != 0) {
        return 4;
    }

    /* Defined success exit status for the KDB lifecycle observation. */
    rc = ow_gate_call(OW_GATE_PS_EXIT, (int64_t)0x5A, 0, 0);
    (void)rc;
    return 0;
}
