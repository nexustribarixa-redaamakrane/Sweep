/* kdb_exec_ps.c - PS-backed associated-kernel executor backend (R5).
 *
 * The management plane (kdb_exec.c) owns the plan; this file owns the only
 * execution backend this build registers.  It binds a KDB instance to a real
 * OpenWindows Process/Thread pair:
 *
 *   OwKdbExecuteInstance()
 *      -> OwKdbBuildLaunchPlan()   (KDB re-validates image + charge + policy)
 *      -> kdb_exec_backend()        (this file; CIS-gated PS load + launch)
 *      -> [scheduler resumes thread] -> OwKdbNotifyThreadDispatch -> RUNNING
 *      -> payload calls bridge / exits    -> OwKdbNotifyThreadExit  -> STOPPED
 *      -> terminal CPL3 fault on bound thr -> OwKdbNotifyThreadFault -> FAULTED
 *
 * Nothing in this file invents an executing state.  A backend success only means
 * a PS process/thread was created, the trusted image mapped into its own guarded
 * user window and the thread enqueued for the PS scheduler; the transition to
 * OW_KDB_INSTANCE_RUNNING happens in the scheduler dispatch hook, which is the
 * first point at which the thread really will run on a CPU.
 *
 * Ownership model: KDB holds *identifiers*, never process pointers.  The binding
 * table stores InstanceId/AssociationId and the PS ProcessId/Tid pair; objects
 * are resolved through the PS accessors at event time, so a terminated process
 * cannot leave a dangling KDB pointer behind.
 */
#include "kdb_internal.h"
#include "../inc/ow_kprintf.h"
#include "../inc/ow_ps.h"

/* One executor binding per instance slot (InstanceId 1..OW_KDB_MAX_INSTANCES). */
typedef struct _OW_KDB_BINDING {
    bool     InUse;
    uint32_t InstanceId;
    uint32_t AssociationId;
    uint32_t ProcessId;
    uint32_t ThreadId;
} OW_KDB_BINDING;

static OW_KDB_BINDING g_Bindings[OW_KDB_MAX_INSTANCES];

/* ---- Binding table maintenance ---------------------------------------- */

void OwKdbExecPsReset(void) {
    uint32_t i;

    for (i = 0U; i < OW_KDB_MAX_INSTANCES; ++i) {
        g_Bindings[i].InUse       = false;
        g_Bindings[i].InstanceId = 0U;
        g_Bindings[i].AssociationId = 0U;
        g_Bindings[i].ProcessId  = 0U;
        g_Bindings[i].ThreadId   = 0U;
    }
}

uint32_t OwKdbExecPsBindingCount(void) {
    uint32_t i;
    uint32_t count = 0U;

    for (i = 0U; i < OW_KDB_MAX_INSTANCES; ++i) {
        if (g_Bindings[i].InUse) {
            ++count;
        }
    }
    return count;
}

bool OwKdbExecPsInstanceBound(uint32_t InstanceId) {
    if (InstanceId == OW_KDB_INVALID_ID || InstanceId > OW_KDB_MAX_INSTANCES) {
        return false;
    }
    return g_Bindings[InstanceId - 1U].InUse;
}

bool OwKdbExecPsGetBindingByThread(uint32_t ThreadId, uint32_t ProcessId,
                                   OW_KDB_BINDING_VIEW* OutView) {
    uint32_t i;

    if (OutView == (OW_KDB_BINDING_VIEW*)0) {
        return false;
    }
    OutView->InUse         = false;
    OutView->InstanceId   = 0U;
    OutView->AssociationId = 0U;
    OutView->ProcessId    = 0U;
    OutView->ThreadId     = 0U;

    for (i = 0U; i < OW_KDB_MAX_INSTANCES; ++i) {
        if (g_Bindings[i].InUse &&
            g_Bindings[i].ThreadId   == ThreadId &&
            g_Bindings[i].ProcessId  == ProcessId) {
            OutView->InUse         = true;
            OutView->InstanceId   = g_Bindings[i].InstanceId;
            OutView->AssociationId = g_Bindings[i].AssociationId;
            OutView->ProcessId    = g_Bindings[i].ProcessId;
            OutView->ThreadId     = g_Bindings[i].ThreadId;
            return true;
        }
    }
    return false;
}

/* ---- Event attribution ------------------------------------------------- */

/* The scheduler is about to iretq into ThreadId.  This is the only place the
 * instance is allowed to leave STOPPED/READY and become RUNNING, and it happens
 * before the actual CPU resume, never during plan validation or preparation. */
bool OwKdbNotifyThreadDispatch(uint32_t ThreadId, uint32_t ProcessId) {
    OW_KDB_BINDING_VIEW view;
    OW_STATUS status;

    if (ThreadId == OW_PS_TID_SYSTEM) {
        return false;
    }
    if (!OwKdbExecPsGetBindingByThread(ThreadId, ProcessId, &view)) {
        return false;
    }
    if (!view.InUse) {
        return false;
    }

    status = OwKdbManagerMarkRunning(view.InstanceId);
    if (ow_status_error(status)) {
        return false;
    }
    return true;
}

/* The bound thread is terminating with ExitStatus.  Retire the instance: stop
 * serving, release the page charge and the association link, release the
 * prepared process frames and clear the binding.  A faulted instance is handled
 * by the fault path instead and is refused here so a double transition cannot
 * happen. */
bool OwKdbNotifyThreadExit(uint32_t ThreadId, uint32_t ProcessId,
                           uint64_t ExitStatus) {
    OW_KDB_BINDING_VIEW view;
    OW_STATUS status;

    if (!OwKdbExecPsGetBindingByThread(ThreadId, ProcessId, &view)) {
        return false;
    }
    if (!view.InUse) {
        return false;
    }
    if (view.InstanceId == OW_KDB_INVALID_ID) {
        return false;
    }

    status = OwKdbManagerFinish(view.InstanceId, ExitStatus);
    if (ow_status_error(status)) {
        return false;
    }

    OwKdbExecPsAbandonInstance(view.InstanceId, ExitStatus);
    return true;
}

/* A terminal CPL3 fault hit a thread that belongs to a live KDB binding.  Fault
 * the owning instance through the normal fault path (which releases frames,
 * pages and the association link and clears the binding) and report true.  A
 * thread without a live binding never reaches this function, so a generic
 * kernel fault is never laundered into a KAPA fault. */
bool OwKdbNotifyThreadFault(uint32_t ThreadId, uint32_t ProcessId,
                            OW_STATUS FaultStatus) {
    OW_KDB_BINDING_VIEW view;
    OW_STATUS status;

    if (ThreadId == OW_PS_TID_SYSTEM) {
        return false;
    }
    if (!OwKdbExecPsGetBindingByThread(ThreadId, ProcessId, &view)) {
        return false;
    }
    if (!view.InUse) {
        return false;
    }
    if (view.InstanceId == OW_KDB_INVALID_ID) {
        return false;
    }

    status = OwKdbFaultInstance(view.InstanceId, FaultStatus);
    if (ow_status_error(status)) {
        return false;
    }
    return true;
}

/* ---- Backend ------------------------------------------------------------- */

/* Called by OwKdbExecuteInstance() with a freshly built, KDB-re-validated plan.
 * Creates the process, maps the trusted image through the CIS-gated PS loader
 * and hands the thread to the PS scheduler.  Nothing here marks RUNNING. */
static OW_STATUS kdb_exec_backend(const OW_KDB_LAUNCH_PLAN* Plan, void* Context) {
    const OW_KDB_KERNEL_DESC* kernel;
    OW_PROCESS_OBJECT* proc;
    OW_THREAD_OBJECT* thread;
    OW_STATUS status;
    uint32_t i;
    const char* argv[1];
    int argc = 1;

    (void)Context;

    if (Plan == (const OW_KDB_LAUNCH_PLAN*)0) {
        return OW_ERR_NULL_POINTER;
    }
    if (Plan->InstanceId == OW_KDB_INVALID_ID ||
        Plan->InstanceId > OW_KDB_MAX_INSTANCES) {
        return OW_ERR_INVALID_PARAM;
    }
    if (g_Bindings[Plan->InstanceId - 1U].InUse) {
        return OW_ERR_ALREADY_EXISTS;
    }

    kernel = OwKdbRegistryPtr(Plan->KernelId);
    if (kernel == (const OW_KDB_KERNEL_DESC*)0) {
        return OW_ERR_NOT_FOUND;
    }
    if (kernel->Image == (const uint8_t*)0 || kernel->ImageSize == 0U) {
        return OW_ERR_NOT_FOUND;
    }
    /* The plan's image size came from the descriptor the plan was built from;
     * the bytes the loader will actually read are the same count.  Cross-check
     * rather than trust, so a descriptor edited between plan and load cannot make
     * the loader walk past its buffer. */
    if ((uint32_t)kernel->ImageSize != Plan->ImageSize) {
        return OW_ERR_INVALID_PARAM;
    }

    proc = OwPsCreateProcess(kernel->Name, OW_PS_PID_KERNEL);
    if (proc == (OW_PROCESS_OBJECT*)0) {
        return OW_ERR_INSUFFICIENT;
    }

    status = OwPsLoadImage(proc, kernel->Image, kernel->ImageSize,
                            OW_CIS_RECORD_SPAWN);
    if (ow_status_error(status)) {
        /* Refusal is always clean (CIS gate before first charge, loader rejects
         * before mapping); release the uncharged process and hand back. */
        OwPsTerminateProcess(proc, (uint64_t)status);
        return status;
    }

    argv[0] = kernel->Name;
    /* The entry thread is created DORMANT and deliberately not published yet:
     * it must not be schedulable until this executor has set the instance
     * binding and the association, or a PIT preemption could enter the payload
     * before the instance is in any state where the gateway or the scheduler
     * attribution can recognize it. */
    status = OwPsLaunchUserImageEx(proc, argc, argv, false);
    if (ow_status_error(status)) {
        OwPsTerminateProcess(proc, (uint64_t)status);
        return status;
    }

    thread = (OW_THREAD_OBJECT*)0;
    for (i = 0U; i < OW_PS_MAX_THREADS; ++i) {
        OW_THREAD_OBJECT* t = OwPsGetThreadByIndex(i);
        if (t && t->Tid == proc->PrimaryThreadId &&
            t->ProcessId == proc->ProcessId) {
            thread = t;
            break;
        }
    }
    if (thread == (OW_THREAD_OBJECT*)0) {
        OwPsTerminateProcess(proc, (uint64_t)OW_ERR_NOT_INITIALIZED);
        return OW_ERR_NOT_INITIALIZED;
    }

    g_Bindings[Plan->InstanceId - 1U].InUse = true;
    g_Bindings[Plan->InstanceId - 1U].InstanceId = Plan->InstanceId;
    g_Bindings[Plan->InstanceId - 1U].AssociationId = Plan->AssociationId;
    g_Bindings[Plan->InstanceId - 1U].ProcessId = proc->ProcessId;
    g_Bindings[Plan->InstanceId - 1U].ThreadId = thread->Tid;

    ow_kprintf("[KDB] instance %u: backend prepared process %u (thread %u) "
               "for '%s'\r\n",
               (unsigned)Plan->InstanceId, (unsigned)proc->ProcessId,
               (unsigned)thread->Tid, kernel->Name);

    status = OwKdbManagerRecordExecution(Plan->InstanceId, proc->ProcessId,
                                        thread->Tid, proc->EntryPoint);
    if (ow_status_error(status)) {
        /* Leave the thread DORMANT; the fault path that this failure triggers
         * owns cleanup (abandon + terminate). */
        return status;
    }

    /* Everything the gateway and the scheduler dispatch attribution need is in
     * place, so the entry thread can finally be published to CPL3. */
    OwPsWakeThread(thread);
    return OW_SUCCESS;
}

/* Install the PS backend.  Called once from OwKdbInitialize(); the backend is
 * the only execution path in this build and it refuses anything the CIS gate
 * did not trust. */
OW_STATUS OwKdbExecPsInitialize(void) {
    return OwKdbRegisterExecutor(&kdb_exec_backend, (void*)0);
}

/* Release ownership of one instance's prepared process without touching KDB
 * instance state (the caller owns the fault/destroy decision).  Used after a
 * normal exit so the finished instance does not keep a binding or frames. */
void OwKdbExecPsAbandonInstance(uint32_t InstanceId, uint64_t ExitStatus) {
    OW_KDB_BINDING* binding;
    uint32_t process_id;
    uint32_t thread_id;

    if (InstanceId == OW_KDB_INVALID_ID || InstanceId > OW_KDB_MAX_INSTANCES) {
        return;
    }
    binding = &g_Bindings[InstanceId - 1U];
    if (!binding->InUse) {
        return;
    }

    process_id = binding->ProcessId;
    thread_id = binding->ThreadId;

    binding->InUse       = false;
    binding->InstanceId = 0U;
    binding->AssociationId = 0U;
    binding->ProcessId  = 0U;
    binding->ThreadId   = 0U;

    if (thread_id != 0U) {
        /* A prepared-but-never-dispatched thread can still sit in the ready
         * queue; retire it so no stale scheduler entry survives abandonment. */
        OW_THREAD_OBJECT* t = (OW_THREAD_OBJECT*)0;
        uint32_t i;

        for (i = 0U; i < OW_PS_MAX_THREADS; ++i) {
            OW_THREAD_OBJECT* cand = OwPsGetThreadByIndex(i);
            if (cand && cand->Tid == thread_id && cand->ProcessId == process_id) {
                t = cand;
                break;
            }
        }
        if (t && t->State == OW_THR_READY) {
            OwPsAbandonThread(t->Tid, t->ProcessId);
        }
    }

    if (process_id != 0U) {
        OW_PROCESS_OBJECT* proc = OwPsGetProcessById(process_id);
        if (proc) {
            OwPsTerminateProcess(proc, ExitStatus);
        }
    }
}
