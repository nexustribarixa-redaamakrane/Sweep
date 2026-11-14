/* kapa_fault.c - controlled CPL3 fault payload for the R6 sweep-kernel test.
 *
 * Its only job is to produce ONE deterministic, attributed fault from inside
 * a genuine KAPA instance, through the real R5/R6 execution path, and have KDB
 * record that fault against that instance.  It is not a crash test of the
 * kernel: the fault is a single not-present read/store at CPL3 in the KAPA's
 * own address space, produced by a plain null pointer dereference, and the
 * KAPA thread dies by the normal fault path.
 *
 * Sequence: print an attributed log line through the Service Bridge, then
 * deliberately dereference an unmapped null address.  The page-fault path
 * classifies it OW_PF_NOTMAPPED, sees the live KDB binding and CPL3, and
 * attributes the fault to the instance; the instance ends FAULTED and every
 * KDB-charged resource for it is released.
 */
#include "../../emergency/ow_gate.h"

void* memcpy(void* dst, const void* src, unsigned long n);
void* memset(void* dst, int c, unsigned long n);

int kapa_fault_main(void);

static void kapa_log(const char* msg) {
    OW_SYS_KDB_BRIDGE_CALL call;
    unsigned long len = 0UL;

    while (msg && msg[len] != '\0') ++len;

    call.Magic = OW_SYS_KDB_BRIDGE_MAGIC;
    call.Version = OW_SYS_KDB_BRIDGE_VERSION;
    call.ServiceId = OW_KDB_SERVICE_LOG;
    call.Flags = 0U;
    call.RequestLength = (uint32_t) len;
    call.ResponseCapacity = 0U;
    call.ResponseLength = 0U;
    call.TargetInstanceId = 0U;
    call.RequestAddress = (uint64_t) (uintptr_t) msg;
    call.ResponseAddress = 0U;
    call.Reserved = 0U;
    (void) ow_gate_bridge(&call);
}

int kapa_fault_main(void) {
    volatile uint64_t* null_page = (volatile uint64_t*) 0x0UL;

    /* Attributed before the fault: proves the bridge was reachable right up
     * to the point the payload crashed. */
    kapa_log("kapa-fault: about to fault deliberately");

    /* Read from the null page.  Produces exactly one CPL3 fault; the kernel
     * kills this thread, attributes it, and never maps anything here. */
    (void) *null_page;

    /* Unreachable: the fault path does not come back.  Keep a defined exit
     * so the image is well-formed even if the fault machinery were absent. */
    (void) ow_gate_call(OW_GATE_PS_EXIT, 0xFFFFFFFF00000000LL, 0, 0);
    return 0xFFFF;
}
