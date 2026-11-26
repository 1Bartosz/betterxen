# dxbc-accc

Classifies D3D shader containers (`DXBC` with `DXIL`, legacy `SHDR`/`SHEX`, and `ACCC` chunks carrying BetterXen's synthetic `FSPX` table) and exposes the C ABI in [`abi/include/bx_shader_bridge.h`](../../../abi/include/bx_shader_bridge.h): `bx_shader_inspect`, `bx_shader_prepare_for_d3dmetal` and `bx_shader_free_buffer`.

## Guest access: `bx_shader_bridge.dll` (#445)

The shims that see shader bytecode (`d3d12_xs.dll`) are x86-64 Windows PE32+ DLLs running inside CrossOver. They cannot load the arm64 macOS library that `cargo build` produces on the host. The bridge reaches guests as a **Windows build of this same crate**, with no second implementation:

- **Build:** `cargo build --target x86_64-pc-windows-gnu` turns the crate's `cdylib` into a PE32+ DLL, published as `bx_shader_bridge.dll`. Its Export Address Table holds exactly the three header functions as direct RVAs. It imports only Windows system DLLs that Wine provides, so no MinGW runtime DLL. `guest/build.sh` enforces both. The allowlist is `KERNEL32`, `ntdll`, `bcryptprimitives`, `USERENV`, `WS2_32` and the `api-ms-win-crt-*` / `api-ms-win-core-*` sets.
- **Callers:** a shim loads the bridge at run time with `GetProcAddress`, so a missing bridge is a logged condition, not a load failure of the shim.
  - **Full path:** it must load the bridge by **full path from its own directory**. That is `GetModuleFileNameW` of the shim's own module (for example via `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)`), plus `bx_shader_bridge.dll`, loaded with `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`.
  - **Why not the bare name:** a bare `LoadLibraryW(L"bx_shader_bridge.dll")` searches the guest EXE's directory first under the [standard search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order). It can therefore miss a bridge staged next to the shims, or pick up a different copy.
  - **Staging:** the bridge is meant to be staged in the same directory as the shims that call it.
- **Why not a host tool over captured logs:** the M2 gate needs the decision at `Create*PipelineState` time inside the guest. That means passing DXIL through and rejecting ACCC-only containers. An offline host tool could count shaders but could not act on them.

### Build and check

```sh
rustup target add x86_64-pc-windows-gnu      # once
shader-transpiler/crates/dxbc-accc/guest/build.sh
target/debug/bx-cli run --prefix /tmp/bx-shader --shims target/bx-shader-bridge \
    --exe target/bx-shader-bridge/check_bridge.exe
```

`guest/build.sh` writes `bx_shader_bridge.dll` and `check_bridge.exe` to `target/bx-shader-bridge/`. It always builds into, and copies from, `<repo>/target`, whatever `CARGO_TARGET_DIR` says, so it cannot package a stale DLL. It resolves `cargo` and `rustc` through rustup, checks that this `rustc` has `std` for the Windows target, and prints which `rustc` it used. It fails if the Export Address Table holds anything other than the three entry points as direct RVAs (an ordinal-only entry or a forwarder fails it), or if any import is outside the allowlist above.

[`guest/check_bridge.c`](guest/check_bridge.c) is a C guest program built against the frozen header. It checks the struct layout with `_Static_assert`, loads the DLL by full path from its own directory, and sends nine containers through the DLL:
- a real DXC SM 6.6 compute blob (from [`d3dmetal-harness`](../d3dmetal-harness/README.md));
- synthetic DXIL pixel SM 6.0 with `RTS0`, once as is and once followed by 16 bytes past its declared size;
- `ACCC` with an `FSPX` v1 stage section, and `ACCC` with an `FSPX` v0 stub;
- `DXIL` + `ACCC`;
- legacy `SHEX`;
- two malformed blobs.

The program prints one `BXSHADER` line per container and a count line, and checks:
- **Classification:**
  - kind, stage and shader model;
  - the declared container size;
  - the exact `DXIL` and `ACCC` chunk sizes and the root signature flag, each derived from the program's own chunk-table walk;
  - the digest, compared with header bytes 4..20 for every container, the real one included.
- **Prepare:** exactly the declared container is copied, never the trailing bytes. ACCC-only and legacy are `BX_SHADER_ERR_UNSUPPORTED`. Malformed input is `BX_SHADER_ERR_INVALID_BLOB`, with output cleared.
- **Free and argument errors:** freeing returned buffers, and null arguments at the DLL boundary.

Recorded run (one Apple M5 Pro, macOS 27.2.1, CrossOver 26.3 `26.3.0.39832`, both a fresh prefix and a D3DMetal bottle):

```text
BXSHADER counts dxil=3 accc_only=2 dual=1 legacy=1 unknown=0 invalid=2
PASS: check_bridge (9 containers through bx_shader_bridge.dll)
```

This is a synthetic, single-host result. It is not a measurement of any title's shader corpus, and not Xbox hardware.

Each of these faults compiled into the DLL fails the check: `dxil_chunk_size` clamped to 1, `accc_chunk_size` multiplied, the digest zeroed, `has_root_signature` set to 7, and the DXIL and ACCC kind numbers swapped. The last one also fails when it is rebuilt with `CARGO_TARGET_DIR` set elsewhere.

### Not done here (other owners)

- **`abi/defs/bx_shader_bridge.def` (`[ABI]`):** the frozen export list for the DLL. Linking with it would also set the export directory's internal name, which is `dxbc_accc.dll` today. Wine loads by file name, so this does not affect loading.
- **Staging in `scripts/build-shims.sh` (Track A):** building the DLL there and verifying it with the other shims, so `build-shims/bin` ships it. That would also make CI build it; today nothing does, and `rustup target add x86_64-pc-windows-gnu` is a manual step.
- **Calling it from `d3d12_xs` at PSO creation, and JSONL telemetry (#466):** today `bx_shader_prepare_for_d3dmetal` reports an ACCC-only container on the guest's stderr (`bx-shader event=prepare status=unsupported …`), not through `bx_log_emit`.
