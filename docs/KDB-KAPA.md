# Kernel Dual Boot (KDB) and Kernel-Associated Application Architecture (KAPA)

## Architecture

- KDB is a kernel subsystem of the main OpenWindows kernel, not a bootloader, not a
  userspace daemon, and not a second-kernel execution host.  It lives under `kdb/` and
  is integrated into the boot sequence as a new phase in `core/main.c` (phase 4d, after
  the process subsystem and before the VFS phase).
- KAPA (Kernel-Associated Application Architecture) is the overall architecture that
  describes an application owning one associated kernel.  KDB is the kernel-side
  management plane that the KAPA application is expressed through: it owns the
  registry, the compatibility policy, the loader front-end, the application
  associations, the instance lifecycle, the Service Bridge and the Sweep reference
  integration.
- An associated kernel is *admitted*, not *executed* by KDB.  This build does not run a
  second kernel on spare CPUs, does not create a VM partition or a flat kernel media for
  one, and does not implement boot adapters for Linux or arbitrary custom kernels.  The
  loader front-end recognizes those kinds and reports them as unsupported rather than
  silently treating them as OpenWindows-derived.

## Scope and honest limitations

What this build implements:

- static, compile-time-sized registry of associated-kernel descriptors;
- compatibility policy with distinct verdicts per axis (architecture, ABI, format,
  memory model, privilege model, kind);
- OWX image validation front-end using the same structural checks and whole-image CRC32c
  verdicts the main loader uses (`ps/owx_loader.c`), plus resident-page accounting;
- application associations between KAPA applications and kernel descriptors;
- instance lifecycle (READY -> RUNNING -> STOPPED -> RUNNING, faults terminal) with
  page budget, logical isolation domain and fault cleanup;
- bounded in-kernel Service Bridge (explicit service IDs, KDB-owned request and response
  copies, capability checks, caller/instance validation, length checks, ownership rules);
- the Sweep reference associated kernel, registered declaratively while no Sweep
  implementation exists in this repository.

What this build does not implement, and therefore does not claim:

- execution of an associated kernel as real second-kernel execution;
- hypervisor/VM/partition support;
- Linux or custom-kernel execution adapters;
- MMU/CPU privilege isolation between the main kernel and associated kernel code.

## Isolation semantics

**R5 note (2026-11-12):** the reduced sweep-kernel profile now *does* execute a controlled KAPA application (`kapa_test.owx`) on the CPU through the PS executor path, with CIS-gated admission, per-instance address space (PS primitives), and KDB lifecycle attribution (`docs/SWEEP_R5_REPORT.md`). This is **not** a second-kernel execution and still not an MMU/CPU privilege boundary — the main kernel and the KAPA payload share one page directory design, one ring, and one fault domain. The exclusions below remain accurate and unclaimed.

KDB's "isolation" is deliberately logical and resource ownership based, not an MMU or CPU
privilege boundary across the executions that happen in this build:

- every instance receives a stable isolation-domain identifier;
- every instance receives a page budget (`OW_KDB_DEFAULT_PAGE_LIMIT` = 1024 pages by
  default, the explicit per-association limit overrides it);
- every instance receives its own service namespace (service IDs are unique within an
  owner, so two instances may own the same ID without colliding).

A descriptor that asks for shared memory or kernel-equivalent privilege is refused by the
compatibility policy instead of being loaded under an isolation assumption KDB cannot keep.

## Public API (`inc/ow_kdb.h`)

- `OwKdbInitialize()` / `OwKdbIsReady()` / `OwKdbGetStats()` — boot phase entry point and
  aggregate accounting;
- registry: `OwKdbRegisterKernel`, `OwKdbUnregisterKernel`, `OwKdbGetKernel`,
  `OwKdbFindKernel`, `OwKdbGetKernelByIndex`, `OwKdbKernelCount`, `OwKdbAssessCompatibility`;
- loader: `OwKdbLoaderSupports`, `OwKdbLoaderValidate`;
- associations: `OwKdbAssociateApplication`, `OwKdbGetAssociation`,
  `OwKdbFindAssociation`, `OwKdbDisassociateApplication`;
- instances: `OwKdbLoadInstance`, `OwKdbStartInstance`, `OwKdbStopInstance`,
  `OwKdbFaultInstance`, `OwKdbDestroyInstance`, `OwKdbGetInstance`, `OwKdbGetInstanceByIndex`;
- Service Bridge: `OwKdbServiceRegister`, `OwKdbServiceUnregister`, `OwKdbBridgeCall`;
- Sweep: `OwKdbRegisterSweep`, `OwKdbAttachSweepImage`.

Internal cross-module interfaces live in `kdb/kdb_internal.h`; the implementation is split
into one translation unit per module (`kdb/kdb_core.c`, `kdb_registry.c`, `kdb_loader.c`,
`kdb_manager.c`, `kdb_assoc.c`, `kdb_bridge.c`, `kdb_sweep.c`) so no module is a single
monolithic file.

## Compatibility matrix

| Axis | Verdict | Meaning |
|---|---|---|
| Kind | `OW_KDB_COMPAT_UNSUPPORTED_KIND` | OpenWindows-derived kinds have backends in this build; Linux / custom report unsupported |
| Architecture | `OW_KDB_COMPAT_ARCH_MISMATCH` | Only x86-64 targets are admitted |
| ABI | `OW_KDB_COMPAT_ABI_MISMATCH` | OpenWindows-derived kinds require their documented ABI; Linux / custom require their documented ABI |
| Format | `OW_KDB_COMPAT_FORMAT_MISMATCH` | OpenWindows requires OWX, Linux requires ELF, custom requires raw |
| Memory model | `OW_KDB_COMPAT_MEMORY_MODEL` | Only private memory is admitted; shared memory is refused |
| Privilege | `OW_KDB_COMPAT_PRIVILEGE_MODEL` | Only unprivileged kernels are admitted |
| Descriptor | `OW_KDB_COMPAT_INVALID_DESCRIPTOR` | Malformed names or image metadata |

A compatible descriptor for a Linux or custom kernel is returned as `OW_KDB_COMPAT_UNSUPPORTED_KIND`
with the reason "this build has no loader adapter for that kernel kind", which distinguishes
"wrong ABI" from "right ABI, not implemented yet".

## Instance lifecycle

```
READY -> RUNNING -> STOPPED -> RUNNING
  |         |          |
  +---------+----------+-------> FAULTED -> destroyed
```

- Loading an association validates the kernel, runs the loader front-end, charges the
  resident pages against the page limit and creates the instance in `READY`.
- `OW_KDB_DEFAULT_PAGE_LIMIT` is used when the caller passes a zero limit; a smaller budget
  than the delivered image is refused with `OW_ERR_INSUFFICIENT`.
- Fault cleanup releases the page charge, clears the association link and unregisters every
  service owned by the instance. The descriptor remains queryable as `FAULTED` until
  `OwKdbDestroyInstance()`.
- One association owns at most one instance.

## Service Bridge

- Service IDs are explicit and unique within their owner; owner `0` is the main
  OpenWindows kernel, a non-zero owner is the associated-kernel instance.
- A handler is invoked through KDB-owned copies: the request is copied into a KDB buffer
  before the handler runs, the handler writes into a KDB-owned response buffer, and a
  successful response is length-checked and copied back to the caller.
- Handlers must not retain any pointer after returning.
- Capability checks enforce the required capability bits per service; a caller that is not
  the owning association needs `OW_KDB_CAP_ADMIN`.
- Calls into a non-zero instance require a `RUNNING` target owned by the caller or admin.

## Sweep reference integration

Sweep is the primary reference associated kernel of the architecture. No Sweep
implementation exists in this repository, so KDB registers a declarative Sweep descriptor
(`OW_KDB_SWEEP_NAME`, stripped OpenWindows kind, compatibility ABI, private memory,
unprivileged) during `OwKdbInitialize()`. Until an image is attached with
`OwKdbAttachSweepImage()`, the descriptor is `DECLARATIVE` and cannot be loaded; loading
returns `OW_ERR_UNSUPPORTED`.

`OwKdbAttachSweepImage()` accepts a real OWX image (the host test uses the validated
`owinit.owx` fixture) so the registry, loader, lifecycle and bridge paths are exercised end
to end.

## Implementation status

- Implemented and host-tested: initialization, registry round trips and duplicate
  rejection, compatibility verdicts, OWX load validation (structure + CRC32c) and page
  accounting, association management, instance lifecycle, page budget, isolation-domain
  assignment, fault cleanup, Service Bridge ownership/bounds/capability rules, built-in
  `main.ping` / `main.echo` services and the Sweep attach path.
- Not implemented, and therefore not claimed: second-kernel execution, VM/partition
  isolation, and Linux/custom-kernel boot adapters.
- KDB boot failure is tolerated by `core/main.c`: it logs
  `OW_W_KDB_UNAVAILABLE` and the boot continues without KAPA associations.

## Diagnostics

- `OW_C_KDB_READY` (`0x0011AC14`) — KDB phase reached its ready state.
- `OW_W_KDB_UNAVAILABLE` (`0x0011A806`) — KDB initialization failed; the boot continues
  without KAPA associations.

## Tests

`tools/hosttest/host_boot.c` contains the `test_kdb()` suite: initialization, Sweep
declarative state, registry duplicates and invalid descriptors, the full compatibility
matrix, loader support/validation and corruption handling, associations, instance lifecycle
and page budget, fault cleanup, Service Bridge built-ins and rules, cross-instance
authorization and the Sweep fixture attach/load path.

Run with `make hosttest` (or `cmake --build <build> --target hosttest && ctest -R hosttest`).
