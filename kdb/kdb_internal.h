/* kdb_internal.h - private interfaces shared by the KDB implementation files.
 *
 * Nothing here is part of the public kernel API.  The split exists so the
 * registry, loader, manager, association table, bridge and Sweep fixture stay
 * separate translation units instead of becoming one large file.
 */
#ifndef OW_KDB_INTERNAL_H
#define OW_KDB_INTERNAL_H

#include "../inc/ow_kdb.h"

/* ---- Small bounded string helpers (kdb_core.c) -------------------------- */
/* Copies Src into Dst[DstSize], always NUL-terminating, and returns true only
 * when the whole source fitted. */
bool OwKdbCopyName(char* Dst, uint32_t DstSize, const char* Src);
bool OwKdbNameEqual(const char* A, const char* B);

/* ---- Registry (kdb_registry.c) ------------------------------------------ */
void         OwKdbRegistryReset(void);
OW_STATUS    OwKdbRegistryStore(const OW_KDB_KERNEL_DESC* Desc,
                                uint32_t* OutKernelId);
OW_STATUS    OwKdbRegistryGet(uint32_t KernelId, OW_KDB_KERNEL_DESC* OutDesc);
OW_STATUS    OwKdbRegistryFind(const char* Name, uint32_t* OutKernelId);
const OW_KDB_KERNEL_DESC* OwKdbRegistryPtr(uint32_t KernelId);
OW_STATUS    OwKdbRegistrySetImage(uint32_t KernelId, const uint8_t* Image,
                                   uint32_t ImageSize);
OW_STATUS    OwKdbRegistryRemove(uint32_t KernelId);
uint32_t     OwKdbRegistryCount(void);

/* ---- Loader (kdb_loader.c) ---------------------------------------------- */
bool         OwKdbLoaderSupportsKind(OW_KDB_KERNEL_KIND Kind);
OW_STATUS    OwKdbLoaderPrepare(const OW_KDB_KERNEL_DESC* Desc,
                                uint32_t* OutPages, uint64_t* OutEntry,
                                uint64_t* OutBase);

/* ---- Associations (kdb_assoc.c) ----------------------------------------- */
void         OwKdbAssociationReset(void);
OW_STATUS    OwKdbAssociationCreate(const char* Application, uint32_t KernelId,
                                    uint32_t Capabilities,
                                    uint32_t* OutAssociationId);
OW_STATUS    OwKdbAssociationGet(uint32_t AssociationId,
                                 OW_KDB_ASSOCIATION* OutAssociation);
OW_STATUS    OwKdbAssociationFind(const char* Application,
                                  OW_KDB_ASSOCIATION* OutAssociation);
OW_STATUS    OwKdbAssociationRemove(uint32_t AssociationId);
bool         OwKdbAssociationSetInstance(uint32_t AssociationId,
                                         uint32_t InstanceId);
bool         OwKdbAssociationKernelInUse(uint32_t KernelId);
uint32_t     OwKdbAssociationCount(void);

/* ---- Manager (kdb_manager.c) -------------------------------------------- */
void         OwKdbManagerReset(void);
uint32_t     OwKdbManagerCount(void);
bool         OwKdbManagerExists(uint32_t InstanceId);
bool         OwKdbManagerIsRunning(uint32_t InstanceId);
OW_STATUS    OwKdbManagerMarkRunning(uint32_t InstanceId);
OW_STATUS    OwKdbManagerFinish(uint32_t InstanceId, uint64_t ExitStatus);
/* Record executor-produced facts (PS ids, real loaded entry/mapped base). */
OW_STATUS    OwKdbManagerRecordExecution(uint32_t InstanceId, uint32_t ProcessId,
                                       uint32_t ThreadId, uint64_t EntryPoint);
void         OwKdbManagerNoteBridgeCall(uint32_t InstanceId);
uint32_t     OwKdbManagerNextDomain(void);
uint32_t     OwKdbManagerFaultCount(void);
uint32_t     OwKdbManagerBridgeCalls(void);

/* ---- Bridge (kdb_bridge.c) ---------------------------------------------- */
typedef struct _OW_KDB_CALL_ATTR {
    uint32_t CallerAssociationId;
    uint32_t TargetInstanceId;
} OW_KDB_CALL_ATTR;

void         OwKdbBridgeReset(void);
OW_STATUS    OwKdbBridgeInitializeBuiltins(void);
void         OwKdbBridgeDropOwner(uint32_t OwnerInstanceId);
uint32_t     OwKdbBridgeServiceCount(void);
uint32_t     OwKdbBridgeCallCount(void);
/* Attribution for the in-progress bridge call (valid only inside a handler on
 * the same single-threaded kernel execution context). */
const OW_KDB_CALL_ATTR* OwKdbBridgeGetCallAttr(void);

/* ---- Executor seam (kdb_exec.c) ------------------------------------------ */
void         OwKdbExecReset(void);

/* ---- PS executor backend (kdb_exec_ps.c) -------------------------------- */
/* Reset clears every binding without touching PS objects; used only during
 * OwKdbInitialize(), before any process can be bound. */
void         OwKdbExecPsReset(void);
/* Install the PS backend.  Called once from OwKdbInitialize(). */
OW_STATUS    OwKdbExecPsInitialize(void);
/* Terminate and unbind one instance's process, if any.  Used by the manager's
 * fault/destroy paths so a caller that tears an instance down does not leave a
 * prepared process running with no owner. */
void         OwKdbExecPsAbandonInstance(uint32_t InstanceId, uint64_t ExitStatus);
/* Number of live instance->process bindings.  Host-test introspection. */
uint32_t     OwKdbExecPsBindingCount(void);
/* True when InstanceId currently owns a binding. */
bool         OwKdbExecPsInstanceBound(uint32_t InstanceId);

/* Read-only view of the binding that owns a PS process/thread.  Used by the
 * reduced-profile syscall gateway to derive the caller's association from the
 * thread it is actually running on. */
typedef struct _OW_KDB_BINDING_VIEW {
    bool     InUse;
    uint32_t InstanceId;
    uint32_t AssociationId;
    uint32_t ProcessId;
    uint32_t ThreadId;
} OW_KDB_BINDING_VIEW;

bool         OwKdbExecPsGetBindingByThread(uint32_t ThreadId, uint32_t ProcessId,
                                           OW_KDB_BINDING_VIEW* OutView);

#endif /* OW_KDB_INTERNAL_H */
