# Sweep

Sweep is an OpenWindows-derived freestanding x86-64 kernel tree and the
primary reference associated-kernel/application target for KDB/KAPA. The
kernel is C99 plus x86 assembly; it builds a PE image packaged in the
project-specific OWX format, and the reduced `sweep-kernel` profile hosts
Sweep as a KAPA application through controlled services. Sweep is not a Linux
compatibility layer, a browser product, or a production-ready general-purpose
operating system.

## Status (R7, 2026-11-14)

| Milestone | Result |
| --- | --- |
| **R7 - native HTML and DOM inside the freestanding KAPA runtime** | Implemented and QEMU-demonstrated. A canned HTML fixture is parsed by Sweep's real `sweep_dom_parse`, built into Sweep's bounded DOM, queried and mutated by Duktape JavaScript at CPL3, and the mutation is verified independently through the C DOM API. `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1` asserts the whole chain (58 assertions). |
| **R6 - real Sweep runtime as a KAPA** | `kapa_sweep.owx` runs the real freestanding runtime: CIS-gated, PIE, 211 base relocations, Duktape evaluates JavaScript at CPL3, the arena drains to zero, and `main.log`/`main.runtime_info` calls are attributed. A separate `kapa_fault.owx` payload demonstrates controlled fault attribution. |
| **R5/R4/R3 - execution foundations** | Controlled KAPA execution, audit/classification, runtime seam (`SWEEP_RUNTIME`), freestanding Duktape, and OWX relocation support. |
| **Hosted Sweep foundations** | `make sweep-tests` 1007 passed, 0 failed (HTML/DOM/text/JS/CSS suites). |
| **Kernel profiles** | Full `openwinkrnl.owx` and reduced `sweepwinkrnl.owx` build and boot; both baselines verified in R7. |
| **KDB/KAPA service bridge** | In-tree builtins `main.ping`/`main.echo`/`main.log`/`main.runtime_info`; `make hosttest` 1424 passed, 0 failed. |

The R7 image is `kapa_sweep.owx` (434,410 B, entry RVA `0x1360`, console
subsystem, SHA-256 `1d99971b…`). Full artifact sizes and hashes are recorded in
`docs/SWEEP_R7_REPORT.md`.

### What is not implemented

No Sweep browser (CSS/layout/render/navigation) is packaged as an OWX KAPA
application. The R7 payload carries the runtime foundation, the real HTML
parser, and the DOM — not the browser. There is no native OWFS resource
backend (`read_resource` is absent from the reduced profile), no JS execution
watchdog while a script runs inside the KAPA, no MMU/CPU/VM isolation, no
second kernel, and no general third-party KAPA support. "Trusted" always means
a CIS verdict, never a hardware boundary. Frames and private page-table pages
of terminated processes are not recycled; KAPA churn is bounded by explicit
limits rather than a free-list allocator.

## Repository layout

| Path | Contents |
| --- | --- |
| `core/`, `hal/`, `ps/`, `syscall/`, `sync/`, `dpc/` | Startup, object/memory, interrupt/device, process/thread, OWX loading, syscall, synchronization |
| `kdb/`, `kapi/` | KDB and the KAPA application interface |
| `shell/`, `rc/`, `vfs/`, `net/`, `storage/` | Shell/runlevel code, VFS facade, routing, storage interfaces |
| `cis/`, `diag/`, `sentinel/`, `bancode/` | Image verification, diagnostics, audition/certification support |
| `sweep/` | Sweep HTML/DOM/text/JS/CSS runtime, freestanding libc shim, KAPA payloads (`sweep/kapa_sweep/`), third-party Duktape |
| `tools/`, `docs/` | Host harnesses, QEMU scripts, generators, and design/evidence documents |

The supported syscall entry is `int 0x80`; see `docs/SYSCALL_ABI.md` for the
register contract. CIS verification is part of the image-loading path. The
default build trusts no signing key; `CIS_DEV_TRUST=1` enables the public,
checked-in development key for test images only and is never a production
trust anchor.

## Repository dependencies

The build files refer to sibling checkouts using `..` paths, so these must be
present:

```text
OpenWindows-Essentials/
OpenWindows-Storage/
superunicode/
vip/
```

The normal boot path also packages
`OpenWindows-Essentials/Artifacts/Software/owinit.owx`. `make
check-external-deps` validates the configured roots; all four profiles are OK
with the default layout and a bogus root fails clearly.

## Build

Requirements: a GCC-compatible C/ASM toolchain, GNU Make, Python 3, and the
sibling repositories above. QEMU is needed for the VM checks; CMake, NASM,
qemu-img, and VirtualBox are needed only for the optional CMake and image
workflows.

```powershell
make -j 2             # full kernel -> openwinkrnl.owx
make sweep-kernel     # reduced Sweep profile -> sweepwinkrnl.owx
make emergency        # emergency image path
```

The CMake build remains available:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure

# Development images signed with the test key only:
cmake -S . -B build-dev -G "MinGW Makefiles" -DCIS_DEV_TRUST=ON
cmake --build build-dev --parallel 2
```

Never ship images signed with the repository's public development key. `make
help` is not defined; the authoritative target list is near the end of
`Makefile`.

## Test and validation

| Target | R7 result |
| --- | --- |
| `make -B -j 2` | Success (full kernel) |
| `make -B sweep-kernel` | Success (reduced profile) |
| `make sweep-tests` | 1007 passed, 0 failed |
| `make hosttest` | 1424 passed, 0 failed |
| `make hosttest-asan` | ASan/UBSan host harness |
| `make sweep-freestanding` | Exit 0, `nm -u` symbol closure OK |
| `make check-external-deps` | All four profiles OK |
| `make qemu-sweep-kernel` | PASS |
| `make qemu-sweep-kapa-test CIS_DEV_TRUST=1` | PASS |
| `make qemu-sweep-kapa-runtime-test CIS_DEV_TRUST=1` | PASS |
| `make qemu-sweep-kapa-fault-test CIS_DEV_TRUST=1` | PASS |
| `make qemu-sweep-kapa-dom-test CIS_DEV_TRUST=1` | PASS (58 assertions) |

`CIS_DEV_TRUST=1` is required for the KAPA QEMU tests because those images are
signed with the development key. Boot logs are written to `build-generated/`,
which is not tracked.

## Documentation

- `SWEEP.md` - current status, evidence, and baselines.
- `docs/SWEEP_R7_REPORT.md` - R7 HTML/DOM/JavaScript evidence and R8 proposals.
- `docs/SWEEP_HTML_DOM.md` - parser, DOM, and JS binding architecture.
- `docs/SWEEP_R6_REPORT.md` - native runtime execution, OWX relocations, fault attribution.
- `docs/SWEEP_R5_REPORT.md`, `docs/SWEEP_R4_REPORT.md`, `docs/SWEEP_R3_CONTINUATION_REPORT.md` - earlier milestone evidence.
- `docs/SWEEP_KAPA_EXECUTION.md`, `docs/SWEEP_KAPA_INTEGRATION.md` - execution audit and integration status.
- `docs/SWEEP_SERVICE_BRIDGE.md`, `docs/SWEEP_BOOT_VALIDATION.md` - service builtins and QEMU evidence.
- `docs/SWEEP_KERNEL_PROFILE.md`, `docs/SWEEP_RUNTIME_PORTABILITY.md` - reduced profile and runtime seam.
- `docs/KDB-KAPA.md`, `docs/CIS.md`, `ROADMAP.md` - KDB/KAPA scope, image verification, and the wider OpenWindows roadmap.

The code and build scripts are the authority for current behavior. VM tests do
not constitute hardware certification or a security audit.

## License

Sweep and the OpenWindows-owned sources in this repository are licensed under
the GNU General Public License, version 3 or any later version
(GPL-3.0-or-later). The complete license text is in `LICENSE`.
Third-party components keep their own licenses; see `THIRD-PARTY-NOTICES.md`.
