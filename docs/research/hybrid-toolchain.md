# Windows cross-toolchain for original workloads (issue #364)

Evidence class B for the build results (commands run on the reference machine, 2026-11-25), class D for licences. Nothing built here was executed: the Windows programs were compiled and inspected, not run, because CrossOver is not installed on that machine.

## Pinned toolchain

| Item | Value |
| --- | --- |
| Toolchain | [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) release `20261117`, asset `llvm-mingw-20261117-ucrt-macos-universal.tar.xz` |
| SHA-256 | `98d340bbb8d46b09ad6886ffba88bd9d5415e131c316de48e6bd7647377446aa`, equal to the digest GitHub shows for the asset; computed locally with `shasum -a 256` after download |
| Host binaries | Mach-O universal (x86_64, arm64); `codesign -dv` shows the clang driver is signed. Runs natively on Apple Silicon |
| Targets used | `x86_64-w64-mingw32` and `aarch64-w64-mingw32` (clang), Rust `x86_64-pc-windows-gnullvm` and `aarch64-pc-windows-gnullvm` |
| Licence | `LICENSE.TXT` in the release opens with "The LLVM Project is under the Apache License v2.0 with LLVM Exceptions" and then lists further component licences (the mingw-w64 runtime has its own terms). The full file was **not** audited line by line. It is an external build tool: nothing from it is shipped or copied into this repository |
| Install | Download, check the digest, unpack outside the repository (for example `~/toolchains/`), pass the directory to the build script. It is not vendored |

Rust 1.100.0 with `rustup target add x86_64-pc-windows-gnullvm aarch64-pc-windows-gnullvm`.

## Build and result

`scripts/hybrid/build-windows-workloads.sh <llvm-mingw-dir>` builds three original programs for both architectures with `-Wall -Wextra -Werror` and then checks each output with `file`; it exits non-zero if a PE has the wrong machine type.

| Program | Source | x86-64 | ARM64 | Executed |
| --- | --- | --- | --- | --- |
| C self-check: CRC-32 check value, four threads, interlocked counter | `workloads/win-selfcheck-c` | built, `PE32+ ... x86-64` | built, `PE32+ ... Aarch64` | no |
| Rust self-check: thread-local storage across four threads | `workloads/win-selfcheck-rust` | built | built | no |
| Direct3D 11 clear and readback of a 64x64 RGBA8 target (first step of #365) | `workloads/d3d11-clear` | built, links `d3d11` and `dxgi` | built | no |

Not demonstrated: that any of these run, pass or render under CrossOver, Wine, Rosetta 2 or D3DMetal. The D3D11 program exits 2 ("no device") rather than passing when no hardware device exists, so a missing GPU path cannot be mistaken for success.

## Alternative evaluated: skakwy's `gnullvm-cc.sh`

The contributor's `tools/macos/gnullvm-cc.sh` (victory-mac commit `22b3f8e45a59`) links `x86_64-pc-windows-gnullvm` with Homebrew clang and rustup's lld. On the reference machine, with Homebrew `llvm@17` as `LLVM_PREFIX`, it failed at link time: `clang-17` aborted with `dyld: Symbol not found: _LLVMInitializeLanaiAsmParser`, because the script sets `DYLD_LIBRARY_PATH` to the Rust toolchain's `libLLVM`. It probably expects a newer Homebrew LLVM; which version was not asked. llvm-mingw covers the same need without that dependency, so the script was **not integrated**. His note that the plain `x86_64-pc-windows-gnu` target aborts on thread start is unverified here, since no program was run.
