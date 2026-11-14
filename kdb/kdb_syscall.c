/* kdb_syscall.c - reduced-profile KAPA gateway for the sweep-kernel profile.
 *
 * The sweep kernel profile installs no full system-call dispatcher: it has no
 * VFS, storage, network or shell, and pulling those in just to serve one KAPA
 * would be the exact "full dispatcher would be simpler" shortcut this profile
 * exists to avoid.  The gateway that vector 0x80 reaches instead is this one,
 * and it serves exactly the KAPA execution surface the first controlled payload
 * needs:
 *
 *   OW_SYS_PS_GET_PID       P1 = P2 = P3 = 0      -> calling process id
 *   OW_SYS_PS_EXIT_THREAD   P1 = exit status       -> retire the calling thread
 *   OW_SYS_UART_WRITE       P1 = string VA         -> debug console write
 *   OW_SYS_KDB_BRIDGE       P1 = OW_SYS_KDB_BRIDGE_CALL VA -> service call
 *
 * Authorization is derived from the calling thread's live KDB binding, never
 * from a caller-supplied identifier, so a CPL3 thread can only ever act on the
 * instance that the executor actually prepared for it.  Every user pointer is
 * validated first (caller VAD tree + OwMemTranslate over the caller's own
 * address space) and only then copied, so no byte of a KAPA request or response
 * is ever read or written without passing the residency checks.
 *
 * Anything else on the gateway's syscall space is refused here; the full
 * dispatcher in syscall/dispatcher.c remains the only implementation for the
 * profiles that installed it.
 */
#include "kdb_internal.h"
#include "../inc/ow_syscall.h"
#include "../inc/ow_hal.h"
#include "../inc/ow_kprintf.h"
#include "../inc/ow_ps.h"
#include "../inc/ow_memory.h"

/* Validate [va, va+len) as resident, user-accessible memory of `proc`'s own
 * address space: inside the guarded user window, covered by a live VAD with the
 * PRESENT and USER bits set, resolvable through the process's own page tables.
 * Returns true when every page in the range passed, false on any refusal. */
static bool kdb_user_range_ok(OW_PROCESS_OBJECT* proc, uint64_t va, uint64_t len) {
    uint64_t end;
    uint64_t off;

    if (len == 0u) {
        return true;
    }
    if (proc == (OW_PROCESS_OBJECT*)0) {
        return false;
    }
    if (proc->Pml4Phys == 0u) {
        return false;
    }
    if (len - 1u > (uint64_t)0xFFFFFFFFFFFFFFFFULL - va) {
        return false;
    }
    end = va + len - 1u;
    if (!OwMemInUserWindow(va) || !OwMemInUserWindow(end)) {
        return false;
    }

    off = 0u;
    while (off < len) {
        uint64_t abs_addr = va + off;
        uint64_t page_base = abs_addr & ~(uint64_t)(OW_PAGE_SIZE - 1u);
        uint64_t page_end = page_base + (uint64_t)OW_PAGE_SIZE - 1u;
        uint64_t last = (va + len - 1u < page_base + (uint64_t)OW_PAGE_SIZE)
                        ? (va + len - 1u) : page_end;
        uint64_t pa = 0;
        uint64_t flags = 0;
        const OW_VAD_NODE* vad;
        uint64_t chunk = (last - abs_addr) + 1u;

        if (ow_status_error(OwMemTranslate(proc->Pml4Phys, abs_addr, &pa, &flags))) {
            return false;
        }
        if ((flags & OW_PAGE_USER) == 0u) {
            return false;
        }
        vad = OwMemFindVad(proc->VadRoot, abs_addr);
        if (vad == (const OW_VAD_NODE*)0) {
            return false;
        }
        if (abs_addr < vad->StartingAddress || last > vad->EndingAddress) {
            return false;
        }
        if ((vad->Protection & OW_PAGE_PRESENT) == 0u) {
            return false;
        }
        if ((vad->Protection & OW_PAGE_USER) == 0u) {
            return false;
        }
        off += chunk;
    }
    return true;
}

/* Copy `len` bytes from the caller's user VA into a kernel-owned buffer. */
static bool kdb_copy_user(OW_PROCESS_OBJECT* proc, uint64_t va, void* dst,
                         uint32_t len) {
    uint8_t* out = (uint8_t*)dst;
    uint64_t off;

    if (len == 0u) {
        return true;
    }
    if (!kdb_user_range_ok(proc, va, (uint64_t)len)) {
        return false;
    }
    for (off = 0u; off < (uint64_t)len; ) {
        uint64_t abs_addr = va + off;
        uint64_t page_base = abs_addr & ~(uint64_t)(OW_PAGE_SIZE - 1u);
        uint64_t page_end = page_base + (uint64_t)OW_PAGE_SIZE - 1u;
        uint64_t last = (va + (uint64_t)len - 1u < page_base + (uint64_t)OW_PAGE_SIZE)
                        ? (va + (uint64_t)len - 1u) : page_end;
        uint64_t pa = 0;
        uint64_t flags = 0;
        uint64_t chunk = (last - abs_addr) + 1u;
        uint32_t k;

        if (ow_status_error(OwMemTranslate(proc->Pml4Phys, abs_addr, &pa, &flags))) {
            return false;
        }
        for (k = 0u; (uint32_t)k < (uint32_t)chunk; ++k) {
            out[(uint32_t)(off + (uint64_t)k)] =
                ((const uint8_t*)(uintptr_t)pa)[k];
        }
        off += chunk;
    }
    return true;
}

/* Copy `len` response bytes back into the caller's user VA. */
static bool kdb_copy_out_user(OW_PROCESS_OBJECT* proc, uint64_t va, const void* src,
                              uint32_t len) {
    const uint8_t* in = (const uint8_t*)src;
    uint64_t off;

    if (len == 0u) {
        return true;
    }
    if (!kdb_user_range_ok(proc, va, (uint64_t)len)) {
        return false;
    }
    for (off = 0u; off < (uint64_t)len; ) {
        uint64_t abs_addr = va + off;
        uint64_t page_base = abs_addr & ~(uint64_t)(OW_PAGE_SIZE - 1u);
        uint64_t page_end = page_base + (uint64_t)OW_PAGE_SIZE - 1u;
        uint64_t last = (va + (uint64_t)len - 1u < page_base + (uint64_t)OW_PAGE_SIZE)
                        ? (va + (uint64_t)len - 1u) : page_end;
        uint64_t pa = 0;
        uint64_t flags = 0;
        uint64_t chunk = (last - abs_addr) + 1u;
        uint32_t k;

        if (ow_status_error(OwMemTranslate(proc->Pml4Phys, abs_addr, &pa, &flags))) {
            return false;
        }
        for (k = 0u; (uint32_t)k < (uint32_t)chunk; ++k) {
            ((volatile uint8_t*)(uintptr_t)pa)[k] =
                in[(uint32_t)(off + (uint64_t)k)];
        }
        off += chunk;
    }
    return true;
}

int64_t OwKdbSyscallDispatch(uint32_t SyscallId, uint64_t P1, uint64_t P2,
                             uint64_t P3) {
    OW_THREAD_OBJECT* thr;
    OW_PROCESS_OBJECT* proc;

    (void)P2;
    (void)P3;

    thr = OwPsGetCurrentThread();
    proc = (thr != (OW_THREAD_OBJECT*)0)
               ? OwPsGetProcessById(thr->ProcessId)
               : (OW_PROCESS_OBJECT*)0;

    switch (SyscallId) {
    case OW_SYS_PS_GET_PID: {
        if (thr == (OW_THREAD_OBJECT*)0) {
            return -1;
        }
        return (int64_t)thr->ProcessId;
    }
    case OW_SYS_PS_EXIT_THREAD: {
        if (thr == (OW_THREAD_OBJECT*)0) {
            return -1;
        }
        OwPsExitThread((uint64_t)P1);
        return 0;
    }
    case OW_SYS_UART_WRITE: {
        const char* s = (const char*)(uintptr_t)P1;
        if (s == (const char*)0) {
            return -1;
        }
        if (thr == (OW_THREAD_OBJECT*)0) {
            return -1;
        }
        OwHalUartWriteString(s);
        return 1;
    }
    case OW_SYS_KDB_BRIDGE: {
        OW_KDB_BINDING_VIEW view;
        OW_SYS_KDB_BRIDGE_CALL call;
        uint8_t request[OW_KDB_BRIDGE_MAX_REQUEST];
        uint8_t response[OW_KDB_BRIDGE_MAX_RESPONSE];
        uint32_t response_length = 0U;
        OW_STATUS status;

        if (thr == (OW_THREAD_OBJECT*)0) {
            return -1;
        }
        if (!OwKdbExecPsGetBindingByThread(thr->Tid, thr->ProcessId, &view) ||
            !view.InUse) {
            ow_kprintf("[KDB] gateway refused bridge: no live KAPA binding "
                       "(tid=%u pid=%u)\r\n",
                       (unsigned)thr->Tid, (unsigned)thr->ProcessId);
            return -1;
        }

        call.Magic = 0U;
        call.Version = 0U;
        call.ServiceId = 0U;
        call.Flags = 0U;
        call.RequestLength = 0U;
        call.ResponseCapacity = 0U;
        call.ResponseLength = 0U;
        call.TargetInstanceId = 0U;
        call.RequestAddress = 0U;
        call.ResponseAddress = 0U;
        call.Reserved = 0U;

        if (!kdb_copy_user(proc, P1, &call, (uint32_t)sizeof(call))) {
            ow_kprintf("[KDB] gateway refused bridge: descriptor not resident\r\n");
            return -1;
        }
        if (call.Magic != OW_SYS_KDB_BRIDGE_MAGIC ||
            call.Version != OW_SYS_KDB_BRIDGE_VERSION) {
            ow_kprintf("[KDB] gateway refused bridge: bad descriptor magic/version "
                       "(magic=0x%X version=%u)\r\n",
                       (unsigned)call.Magic, (unsigned)call.Version);
            return -1;
        }
        if (call.Flags != 0U || call.Reserved != 0U ||
            call.TargetInstanceId != 0U) {
            ow_kprintf("[KDB] gateway refused bridge: unsupported descriptor "
                       "flags/reserved/target (flags=0x%X reserved=0x%llX "
                       "target=%u)\r\n",
                       (unsigned)call.Flags,
                       (unsigned long long)call.Reserved,
                       (unsigned)call.TargetInstanceId);
            return -1;
        }
        if (call.RequestLength > OW_KDB_BRIDGE_MAX_REQUEST ||
            call.ResponseCapacity > OW_KDB_BRIDGE_MAX_RESPONSE) {
            ow_kprintf("[KDB] gateway refused bridge: descriptor length over "
                       "the bridge bound (req=%u resp=%u)\r\n",
                       (unsigned)call.RequestLength, (unsigned)call.ResponseCapacity);
            return -1;
        }
        if (call.RequestLength > 0U && call.RequestAddress == 0U) {
            ow_kprintf("[KDB] gateway refused bridge: request length without "
                       "address\r\n");
            return -1;
        }
        if (call.RequestLength == 0U && call.RequestAddress != 0U) {
            ow_kprintf("[KDB] gateway refused bridge: request address without "
                       "length\r\n");
            return -1;
        }
        if (call.ResponseCapacity > 0U && call.ResponseAddress == 0U) {
            ow_kprintf("[KDB] gateway refused bridge: response capacity without "
                       "address\r\n");
            return -1;
        }
        if (call.ResponseCapacity == 0U && call.ResponseAddress != 0U) {
            ow_kprintf("[KDB] gateway refused bridge: response address without "
                       "capacity\r\n");
            return -1;
        }

        if (!kdb_user_range_ok(proc, call.RequestAddress, call.RequestLength) ||
            !kdb_user_range_ok(proc, call.ResponseAddress, call.ResponseCapacity)) {
            ow_kprintf("[KDB] gateway refused bridge: caller memory not resident "
                       "(req=%llX len=%u resp=%llX cap=%u)\r\n",
                       (unsigned long long)call.RequestAddress,
                       (unsigned)call.RequestLength,
                       (unsigned long long)call.ResponseAddress,
                       (unsigned)call.ResponseCapacity);
            return -1;
        }
        if (call.RequestLength > 0U &&
            !kdb_copy_user(proc, call.RequestAddress, request, call.RequestLength)) {
            ow_kprintf("[KDB] gateway refused bridge: request copy failed\r\n");
            return -1;
        }

        status = OwKdbBridgeCall(view.AssociationId, 0U, call.ServiceId, request,
                                 call.RequestLength, response,
                                 OW_KDB_BRIDGE_MAX_RESPONSE, &response_length);
        if (ow_status_error(status)) {
            ow_kprintf("[KDB] gateway bridge call FAILED (st=0x%X service=0x%X "
                       "assoc=%u)\r\n",
                       (unsigned)status, (unsigned)call.ServiceId,
                       (unsigned)view.AssociationId);
            return -1;
        }
        if (response_length > call.ResponseCapacity ||
            response_length > OW_KDB_BRIDGE_MAX_RESPONSE) {
            ow_kprintf("[KDB] gateway bridge response OVERRUN (produced=%u "
                       "cap=%u)\r\n",
                       (unsigned)response_length, (unsigned)call.ResponseCapacity);
            return -1;
        }
        if (call.ResponseAddress != 0U &&
            !kdb_copy_out_user(proc, call.ResponseAddress, response, response_length)) {
            ow_kprintf("[KDB] gateway refused bridge: response copy failed\r\n");
            return -1;
        }

        return 0;
    }
    default:
        /* Not part of the reduced KAPA surface. */
        return -1;
    }
}
