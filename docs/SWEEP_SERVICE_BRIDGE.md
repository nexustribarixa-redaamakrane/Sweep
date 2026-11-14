# Sweep Service Bridge Architecture (R2 + R3/R4, R5 gateway, R6 evidence)

**Date:** 2026-11-14 (R3 MVP; R4 verification refresh; R5 gateway evidence; R6/R7 runtime calls demonstrated)  
**Status:** Kernel KDB bridge primitives and main-kernel builtins implemented, including the R3 MVP services. The R3 hosttest additions now run to completion: `make hosttest` **1424 passed, 0 failed**. The R6 KAPA runtime calls `main.log` and `main.runtime_info` from CPL3 with attribution — observed in QEMU (`[KDB][LOG] assoc=1 target=0 len=51: kapa-sweep: runtime initialized, arena 196608 bytes`) and asserted by `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1`. The R7 KAPA runtime calls the same two services around a full HTML-DOM-JS round trip — observed in QEMU (`build-generated/sweep_kapa_dom_qemu_boot.log`). Sweep-specific runtime services remain planned; the main-kernel service surface exposed to KAPAs is deliberately minimal (`main.ping`, `main.log`, `main.runtime_info`, `main.echo`).

## 1. KDB Service Bridge (source-verified)

**Implementation:** `kdb/kdb_bridge.c`, `kdb/kdb_internal.h`, `inc/ow_kdb.h`

- **Model:** bounded in-kernel call path (not a syscall, not CPL3 boundary). `OwKdbBridgeCall(caller_assoc, target_instance, service_id, request, req_len, response, resp_cap, &out_len)`.
- **Request handling:** caller request copied into KDB-owned buffer (`OW_KDB_BRIDGE_MAX_REQUEST`, 256 B) before handler runs.
- **Response handling:** handler writes into KDB-owned buffer (`OW_KDB_BRIDGE_MAX_RESPONSE`, 256 B) with declared capacity; on success KDB validates produced length against handler/caller capacities and copies back to caller buffer. No caller-owned pointers passed to handlers; handlers must not retain pointers.
- **Registration:** `OwKdbServiceRegister(owner_instance, service_id, name, required_caps, max_request, max_response, handler, context)` with limits bounded by bridge maxima; services unique per `(owner, service_id)`; owner instance must exist when non-zero.
- **Authorization/routing:** caller must be live association with `OW_KDB_CAP_CALL`; calling into target instance requires target exists and is **RUNNING** (logical KDB state) and caller owns instance or has `OW_KDB_CAP_ADMIN`; service must belong to target; required capabilities subset of caller capabilities enforced.
- **Accounting/cleanup:** bridge/instance call counts tracked; `OwKdbBridgeDropOwner` removes instance-owned services on fault/destroy paths.
- **Call attribution (R3):** bridge sets in-progress attribution (`CallerAssociationId`, `TargetInstanceId`) for handler use on the single-threaded kernel call path and clears it after dispatch; used by `main.log` for bounded attribution.

### Built-in main-kernel services (owner 0)

| Service | ID | Caps | Max req | Max resp | Contract | Status |
|---|---:|---|---:|---:|---|---|
| `main.ping` | `0x4B440001` (`OW_KDB_SERVICE_PING`) | `OW_KDB_CAP_CALL` | 0 | 16 | Returns `"kdb-pong"` (8 bytes) | **Implemented/preserved, existing tests** |
| `main.echo` | `0x4B440002` (`OW_KDB_SERVICE_ECHO`) | `OW_KDB_CAP_CALL` | 64 | 64 | Echoes request bytes (bounded) | **Implemented/preserved, existing tests** |
| `main.log` | `0x4B440003` (`OW_KDB_SERVICE_LOG`) | `OW_KDB_CAP_CALL` | 128 | 0 | Request: message bytes (not required NUL-terminated), bounded copy; Response: none (`len=0`). Emits attributed diagnostic via `ow_kprintf` with fixed format (`assoc`, `target`, `len`, message bytes as data — **no format-string interpretation of request**). Invalid/oversized requests refused by bridge/service bounds/caps/validation. | **Implemented (R3); hosttest PASS** |
| `main.runtime_info` | `0x4B440004` (`OW_KDB_SERVICE_RUNTIME_INFO`) | `OW_KDB_CAP_CALL` | 0 | 8 | Request: empty. Response: 8-byte little-endian `uint64_t` uptime in milliseconds from `OwHalUptimeMs()` (HAL monotonic 64-bit ms uptime; **not wall-clock time**). Insufficient response capacity refused; non-empty request refused. | **Implemented (R3); hosttest PASS** |

**Classification:** Bridge mechanism and builtins **implemented and hosttest-verified**. It is deliberately **not** a general runtime service ABI for a natively executing Sweep KAPA app (a minimal 4-service surface plus PS exit/PID is all a KAPA can reach in R5).

## 1b. R5 reduced-profile gateway (how KAPAs reach the bridge)

The sweep-kernel profile does not install the full syscall dispatcher. Vector `0x80` (unchanged IDT gate, DPL3) is routed to `OwKdbSyscallDispatch` (`kdb/kdb_syscall.c`), which serves exactly:

| Call | Number | Semantics |
|---|---|---|
| `OW_SYS_PS_EXIT_THREAD` | `0x00E` | P1 = exit status (low 32 bits meaningful); retires the calling CPL3 thread with that status |
| `OW_SYS_PS_GET_PID` | `0x00F` | returns the calling process id |
| `OW_SYS_UART_WRITE` | `0x01B` | P1 = user string VA; bounded console write (borrowed from the emergency/runtime debug path) |
| `OW_SYS_KDB_BRIDGE` | `0x022` | P1 = user VA of an `OW_SYS_KDB_BRIDGE_CALL`; the bounded bridge call described below |

`OW_SYS_KDB_BRIDGE_CALL` is a fixed-layout descriptor (`inc/ow_syscall.h`): `Magic` `0x4B425247` (`"KBRG"`), `Version` `1`, `ServiceId`, `Flags` (must be 0), `RequestLength`/`ResponseCapacity` (≤ `OW_KDB_BRIDGE_MAX_*`), `ResponseLength` (out), `TargetInstanceId` (**must be 0** = main kernel in R5), `RequestAddress`/`ResponseAddress`, `Reserved` (must be 0). `TargetInstanceId != 0` is refused; cross-instance calls are not exposed to KAPAs.

Authorization is derived from the calling thread's live KDB binding (`OwKdbExecPsGetBindingByThread`), never from a caller-supplied identifier. Every user pointer is validated before any byte is copied: caller VAD tree (PRESENT+USER) + `OwMemTranslate` over the caller's own PML4 + the guarded user window bounds; non-resident user pages are refused (no demand paging from kernel context). The request is copied into a KDB-owned buffer before the bridge runs; the response is copied back only after the bridge returns success and the produced length is checked against the caller's `ResponseCapacity`.

QEMU evidence (R5): `[KDB][LOG] assoc=1 target=0 len=16: kapa-r5-entry-ok` — the KAPA payload called `main.log` from CPL3 and the bridge attributed it to its association (`assoc=1`) on target `0` (main kernel).

## 2. Existing Sweep host-side service handlers (app/runtime layer)

**Implementation:** `sweep/sweep_services.h`, `sweep/sweep_services.c`, `sweep/sweep_kdb_register.c`, tests in `sweep/tests/sweep_services_test.c` (**18 passed, 0 failed** as part of `make sweep-tests`).

These follow `OW_KDB_SERVICE_HANDLER`-style contract semantics (bounded request/response, no retained pointers) and are unit-testable on host; registration helper targets a live KDB instance owner.

| Service | ID | Request (max/format) | Response | Purpose | Status |
|---|---:|---|---|---|---|
| `sweep.url.admit` | `0x53570001` | URL bytes (length provided) | admitted flag + host bytes/NUL (bounded by capacity) | URL admission policy helper | **Implemented and host-tested** |
| `sweep.viewport` | `0x53570002` | 12 bytes: 3×u32 LE (w,h,scale) | 12 bytes: negotiated 3×u32 LE | Viewport negotiation/clamping | **Implemented and host-tested** |
| `sweep.paint.append` | `0x53570003` | 29-byte paint command wire form | u32 accepted (0/1) | Bounded paint session append | **Implemented and host-tested** |

**Gap:** these are not wired into a running native Sweep KAPA process on `sweepwinkrnl`; no kernel-side registration/runtime exposure for an executing Sweep app demonstrated.

## 3. R2 service inventory (specified for Sweep KAPA runtime)

Only services supportable by existing primitives and safe bounded contracts should be implemented; others remain planned/documented.

| Service | Purpose | Proposed request/response (bounded) | Caps/validation | Ownership/copying | Status |
|---|---|---|---|---|---|
| Logging/diagnostics | Bounded diagnostic messages from app/runtime | req: level + UTF-8/byte msg (bounded, e.g. ≤ bridge max); resp: status/accepted bytes | `CALL`; validate lengths/levels; rate/size bounds | KDB copies in/out; no retained pointers | **Implemented as `main.log` (bounded message bytes; no level byte); hosttest PASS** |
| Runtime info | Kernel/KAPA/profile/ABI info query | req: info selector; resp: fixed/bounded struct or TLV | `CALL` | KDB-owned buffers | **Implemented as `main.runtime_info` (monotonic uptime; no selector); hosttest PASS** |
| Timers/monotonic time | Tick/deadline/uptime queries | req: op; resp: u64 tick/ms (bounded) | `CALL`; read-only | copied values only | **Planned/spec only** (HAL/PS ticks exist; service wrapper not implemented) |
| Memory (runtime-managed) | Bounded alloc/release within instance budget | req: op/size/alignment/token; resp: handle/offset/status | `CALL` (+admin as needed); strict budget/ownership checks | no raw kernel pointers to app; handles/tokens only | **Planned/spec only** (logical budgets exist; runtime allocator service not implemented) |
| Resource mediation | Controlled fetch of allowed assets | req: resource id/path token (policy-bound); resp: bounded bytes/metadata or handle | `CALL` + resource caps; root confinement, size/MIME limits | mediated copies/handles, no arbitrary FS | **Planned/spec only** |
| Lifecycle | Controlled exit/stop/fault notification | req: code/reason; resp: ack/status | `CALL`; validates state transitions | kernel owns transition/cleanup | **Planned/spec only** |
| Input events | Ordered bounded event delivery | req: poll/ack; resp: event records | `CALL` + input cap | bounded queues/copies | **Planned/spec only** |
| Surface/presentation handoff | Submit bounded surface to trusted presenter | req: surface descriptor/handle; resp: status | `CALL` + presentation cap | no raw framebuffer export | **Unsupported/Planned** (no real presentation API) |
| Networking | Policy-controlled network access | N/A in R2 | explicit policy caps | mediated only | **Unsupported/Planned** |

## 4. Security and correctness rules (mandatory)

- No unrestricted kernel memory, physical memory, or device access exposed via services.
- Browser scripts/content must not receive kernel/KDB capabilities; services are runtime/app-boundary mechanisms only.
- All requests/responses bounded (`OW_KDB_BRIDGE_MAX_REQUEST/RESPONSE` hard maxima in KDB; per-service maxima may be lower).
- Validate service IDs, capability bits, target ownership/state, structure lengths, and app-controlled identifiers.
- Explicit ownership/copying: handlers operate on KDB-owned buffers; caller buffers written only on validated success.
- Fail closed on invalid/oversized/missing-capability/exhaustion cases; deterministic cleanup on instance fault/destroy.

## 5. Testing status (R3/R4)

- **Verified:** `make hosttest` **1424 passed, 0 failed**, including focused Service Bridge tests in `tools/hosttest/host_boot.c`:
  - 4 built-ins registered (`main.ping`, `main.echo`, `main.log`, `main.runtime_info`).
  - `main.log`: empty request, one byte, normal message, 128-byte maximum, oversized request, missing `OW_KDB_CAP_CALL`, unknown caller, unknown target, repeated calls, percent-data output capture (no format interpretation), attribution cleared after success and failure.
  - `main.runtime_info`: valid 8-byte LE uptime, repeated monotonic reads, insufficient capacity, non-empty request, missing `CAP_CALL`, unknown caller, unknown target, attribution cleared.
  - Handler failure (`OW_ERR_IO`) and overrun (claims capacity+1) propagation: no partial copy, attribution cleanup, zero-length requests, overrun rejection.
- **Runtime integration on `sweepwinkrnl`:** **added in R5** — the KAPA self-test payload calls `main.log` through `OW_SYS_KDB_BRIDGE` and `main.ping`, both observed in the boot log; `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` asserts the whole chain. Host-side `kdb_syscall.c` is also exercised by the executor-seam host tests.
- **Required for future implementation:** valid/invalid IDs, missing caps, oversized req/resp, malformed structures, ownership/copying, exhaustion, repeated calls, failure/recovery, lifecycle interactions for Sweep-specific services (memory/resource/input/presentation).

**Classification:** KDB bridge core - **Implemented**; Sweep host handlers - **Implemented and host-tested**; R5 reduced-profile KAPA gateway (`OW_SYS_KDB_BRIDGE`) - **Implemented and QEMU-validated**; native Sweep KAPA service runtime - **Specified/Planned, not implemented**.
