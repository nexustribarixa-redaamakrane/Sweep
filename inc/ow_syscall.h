/* ow_syscall.h - System Call Gateway (Ring 3 to Ring 0) */
#ifndef OW_SYSCALL_H
#define OW_SYSCALL_H

#include "ow_types.h"
#include "ow_kdb.h"

#define OW_SYS_ALLOC            0x001U
#define OW_SYS_MAP_IO           0x002U
#define OW_SYS_VFS_CREATE       0x003U
#define OW_SYS_VFS_WRITE        0x004U
#define OW_SYS_VFS_READ         0x005U
#define OW_SYS_ALPC_CREATE      0x006U
#define OW_SYS_ALPC_SEND        0x007U
#define OW_SYS_ALPC_RECV        0x008U
#define OW_SYS_VFS_DELETE       0x009U
#define OW_SYS_OBJ_OPEN         0x00AU
#define OW_SYS_OBJ_CLOSE        0x00BU
#define OW_SYS_SAFE_POWEROFF_SCREEN 0x00CU
#define OW_SYS_PS_YIELD         0x00DU
#define OW_SYS_PS_EXIT_THREAD   0x00EU
#define OW_SYS_PS_GET_PID       0x00FU
#define OW_SYS_PS_CREATE_PROCESS 0x010U
#define OW_SYS_PS_CREATE_THREAD 0x011U
#define OW_SYS_SYNC_CREATE_EVENT       0x012U
#define OW_SYS_SYNC_SET_EVENT          0x013U
#define OW_SYS_SYNC_RESET_EVENT        0x014U
#define OW_SYS_SYNC_CREATE_SEMAPHORE   0x015U
#define OW_SYS_SYNC_RELEASE_SEMAPHORE  0x016U
#define OW_SYS_SYNC_WAIT               0x017U
#define OW_SYS_PS_SLEEP                0x018U
#define OW_SYS_SYNC_CREATE_MUTEX       0x019U
#define OW_SYS_SYNC_RELEASE_MUTEX      0x01AU
#define OW_SYS_UART_WRITE              0x01BU
#define OW_SYS_SET_AUTOREBOOT          0x01CU
#define OW_SYS_SET_KDUMP               0x01DU
#define OW_SYS_GET_AUTOREBOOT   0x01EU
#define OW_SYS_GET_KDUMP        0x01FU

/* OW_SYS_VFS_READ (0x005) contract.
 *
 *   P1 = NUL-terminated file name in the calling process's own memory
 *   P2 = writable destination buffer in the calling process's own memory
 *   P3 = destination capacity in bytes
 *   returns, on success, the number of bytes copied (>= 0).
 *
 * This reads from the primary OWFS volume -- the filesystem that carries the
 * real user-mode images and data -- not from the in-kernel legacy VFS that the
 * shell's `write`/`read` verbs use.  Access is decided by the OWFS permission
 * model against the calling PROCESS's identity (OW_PROCESS_OBJECT.Uid/Gid), not
 * against the kernel's ambient identity, so an unprivileged process is refused a
 * file its mode denies it, and cannot even name a file flagged OWFS_SEC_HIDDEN.
 * Both the name and the buffer are validated against this process's own VAD tree
 * before either is touched.
 *
 * On refusal the return is one of the negative OW_SYS_VFS_E* codes below (or the
 * generic -1 the dispatcher uses for an unknown/not-ready request).  They are
 * distinct so a Ring 3 caller can tell a permission refusal from a bad pointer
 * from a missing file from a name that exceeded the gateway's bounded scan. */
#define OW_SYS_VFS_EINVAL       (-2)   /* NULL name/buffer, or cap == 0        */
#define OW_SYS_VFS_EFAULT       (-3)   /* name/buffer not mapped for this proc */
#define OW_SYS_VFS_EACCES       (-4)   /* OWFS permission denied (mode)        */
#define OW_SYS_VFS_ENOENT       (-5)   /* no such file, or hidden from caller  */
#define OW_SYS_VFS_EIO          (-6)   /* backend I/O error / corrupt volume   */
#define OW_SYS_VFS_ENAMETOOLONG (-7)   /* name did not fit the user ceiling    */

/* Load and enter a named OWX1 image from the primary OWFS volume as a new
 * process.  This is what makes the emergency userspace work: owinitv.owx is
 * entered as PID 1 by the kernel, and it is owinitv -- a user-mode program, like
 * any other -- that spawns owrs.owx through this call.
 *
 *   P1 = NUL-terminated image name in the calling process's own memory
 *   P2 = 0
 *   P3 = 0
 *   returns the new process id, or -1 on failure.
 *
 * The name is copied with an explicit bound (OW_USERSPACE_NAME_MAX) before any
 * use: an unbounded walk over a caller-supplied pointer has no termination
 * guarantee, which is a hazard the rest of this gateway does not have and this
 * call does not get to inherit.  The image is then held to exactly the gate
 * OwPsLoadImage applies, so the spawned process is either entered or refused
 * with a reason -- never mapped and left to fault at CPL3. */
#define OW_SYS_PS_SPAWN_OWX            0x020U

/* Poll the debug console for one input byte.
 *   P1..P3 = 0
 *   returns the byte (0..255), or -1 when nothing is waiting.
 *
 * Non-blocking by contract: the underlying OwHalUartReadChar() spins until a
 * byte arrives, so the gateway must gate on OwHalUartCanRead() first or it
 * would park a preemptible CPL3 thread inside the kernel with no way to yield.
 * A caller polls and sleeps between polls. */
#define OW_SYS_UART_READ               0x021U

/* ---- KAPA control/service gateway (R5, reduced profile) ------------------
 * The sweep-kernel profile does not install the full system-call dispatcher:
 * it has no VFS, storage, network or shell.  A KAPA thread that has been
 * bound to a KDB instance gets a deliberately small gateway instead:
 *
 *   OW_SYS_PS_EXIT_THREAD   P1 = exit status; terminates the calling thread
 *   OW_SYS_PS_GET_PID       returns the calling process id
 *   OW_SYS_KDB_BRIDGE       P1 = user VA of one OW_SYS_KDB_BRIDGE_CALL
 *
 * Authorization is derived from the calling thread's KDB binding, never from
 * a user-supplied id.  Only main-kernel services (TargetInstanceId == 0) are
 * reachable in R5; cross-instance calls stay in the kernel-side bridge API.
 * Every user pointer is first proven to be a resident, user-accessible page
 * in the caller's own VAD tree and page tables before a byte is copied.
 *
 * The descriptor is a fixed-layout struct with explicit 32/64-bit fields so
 * a freestanding OWX image can build it without depending on compiler
 * padding.  All reserved fields must be zero. */
#define OW_SYS_KDB_BRIDGE              0x022U

#define OW_SYS_KDB_BRIDGE_MAGIC        0x4B425247U  /* 'KBRG' */
#define OW_SYS_KDB_BRIDGE_VERSION      1U

typedef struct _OW_SYS_KDB_BRIDGE_CALL {
    uint32_t Magic;             /* OW_SYS_KDB_BRIDGE_MAGIC                     */
    uint32_t Version;           /* OW_SYS_KDB_BRIDGE_VERSION                   */
    uint32_t ServiceId;         /* OW_KDB_SERVICE_*                            */
    uint32_t Flags;             /* must be 0                                   */
    uint32_t RequestLength;     /* <= OW_KDB_BRIDGE_MAX_REQUEST                */
    uint32_t ResponseCapacity;  /* <= OW_KDB_BRIDGE_MAX_RESPONSE               */
    uint32_t ResponseLength;    /* out: bytes written by the service           */
    uint32_t TargetInstanceId;  /* must be 0 (main kernel) in R5               */
    uint64_t RequestAddress;    /* user VA; 0 only when RequestLength == 0     */
    uint64_t ResponseAddress;   /* user VA; 0 only when Capacity == 0          */
    uint64_t Reserved;          /* must be 0                                   */
} OW_SYS_KDB_BRIDGE_CALL;

OW_STATUS    OwSyscallInitialize(void);
int64_t      OwSyscallDispatch(uint32_t SyscallId, uint64_t P1, uint64_t P2, uint64_t P3);

/* Reduced-profile gateway.  Lives in kdb/kdb_syscall.c and is wired to vector
 * 0x80 only when OW_SWEEP_KERNEL is defined.  Returns 0 on success and -1 on
 * any refusal; refusals are logged with their OW_STATUS for diagnosis. */
int64_t      OwKdbSyscallDispatch(uint32_t SyscallId, uint64_t P1, uint64_t P2,
                                  uint64_t P3);

/* User-mode wrappers */
void*        OwApiAllocateMemory(uint64_t Size);
OW_STATUS    OwApiCreateFile(const char* Path, uint32_t Attrs);
OW_STATUS    OwApiWriteFile(const char* Path, const uint8_t* Buffer, uint32_t Size);
uint32_t     OwApiReadFile(const char* Path, uint8_t* Buffer, uint32_t MaxSize);
uint64_t     OwApiAlpcCreatePort(const char* Name, bool IsServer);
OW_STATUS    OwApiAlpcSend(uint64_t PortHandle, const void* Data, uint32_t Size);
OW_STATUS    OwApiAlpcRecv(uint64_t PortHandle, void* Buffer, uint32_t MaxSize, uint32_t* OutSize);
OW_STATUS    OwApiSetSafePowerOffScreen(bool Enable);
OW_STATUS    OwApiSetAutoReboot(bool Enable);
OW_STATUS    OwApiSetKdump(bool Enable);
bool         OwApiGetAutoReboot(void);
bool         OwApiGetKdump(void);
OW_STATUS    OwApiPsYield(void);
OW_STATUS    OwApiPsExitThread(void);
uint32_t     OwApiPsGetPid(void);
uint64_t     OwApiPsCreateProcess(const char* Name, uint32_t ParentPid);
uint64_t     OwApiPsCreateThread(uint64_t ProcessHandle,
                                 uint64_t EntryFunction,
                                 uint64_t EntryArg);
uint64_t     OwApiPsSpawnOwx(const char* ImageName);
int          OwApiUartRead(void);

/* Dispatcher object wrappers */
uint64_t     OwApiSyncCreateEvent(const char* Name, uint8_t ManualReset,
                                  uint8_t InitialState);
OW_STATUS    OwApiSyncSetEvent(uint64_t Handle);
OW_STATUS    OwApiSyncResetEvent(uint64_t Handle);
uint64_t     OwApiSyncCreateSemaphore(const char* Name, uint32_t InitialCount,
                                      uint32_t Limit);
OW_STATUS    OwApiSyncReleaseSemaphore(uint64_t Handle, uint32_t Count);
uint32_t     OwApiSyncWait(uint64_t Handle, uint64_t TimeoutTicks);
uint64_t     OwApiSyncCreateMutex(const char* Name, uint8_t InitialOwner);
OW_STATUS    OwApiSyncReleaseMutex(uint64_t Handle);
OW_STATUS    OwApiSleep(uint64_t Ticks);

#endif /* OW_SYSCALL_H */
