# Third-Party Notices

Sweep is derived from the OpenWindows kernel. OpenWindows-owned and
Sweep-specific sources in this repository are licensed under the GNU General
Public License, version 3 or any later version (GPL-3.0-or-later); see
`LICENSE` for the complete text and the copyright preamble.

Sweep also uses sibling projects and generated inputs that remain subject to
their own licenses. The GPL-3.0-or-later license for OpenWindows-owned code
does not automatically relicense these components.

| Component | Location or role | License |
| --- | --- | --- |
| OpenWindows kernel / Sweep sources | `core/`, `hal/`, `ps/`, `syscall/`, `sweep/`, `tools/`, and the rest of this repository except third-party directories | GPL-3.0-or-later; see `LICENSE` |
| SuperUnicode / SUCS / SUTF | `../superunicode/` and imported `sutf` sources | MIT **or** Apache-2.0, at the recipient's option; see `../superunicode/LICENSE`, `LICENSE-MIT`, and `LICENSE-APACHE` |
| OpenWindows-Storage | `../OpenWindows-Storage/` and imported OWFS/USFS sources | MIT **or** Apache-2.0, at the recipient's option; see `../OpenWindows-Storage/LICENSE`, `LICENSE-MIT`, and `LICENSE-APACHE` |
| UniVIP / FVIP | `../vip/` and imported VIP sources | See the dependency's license and notices before redistribution |
| OpenWindows-Essentials `owinit` | `../OpenWindows-Essentials/` and the generated `boot/owinit_image.h` | See the Essentials project's license and notices |
| GCC, Python, CMake, QEMU, VirtualBox, NASM | Build and validation tools | Each tool retains its own license; these tools are not relicensed by OpenWindows |
| Duktape 2.7.0 | `sweep/third_party/duktape/` (`duktape.c`, `duktape.h`, `duk_config.h`, `LICENSE.txt`, `README.rst`); embedded ECMAScript engine behind `sweep/sweep_js_engine.{h,c}` | MIT License; see `sweep/third_party/duktape/LICENSE.txt`. One Sweep-patched definition in `duk_config.h` (`DUK_USE_INTERRUPT_COUNTER` + `SWEEP timeout hook`) with the original define retained in a comment and the failure mode documented in `docs/SWEEP_JS.md`. |

When publishing source or binary releases, include the applicable third-party
license files and notices alongside the corresponding components.