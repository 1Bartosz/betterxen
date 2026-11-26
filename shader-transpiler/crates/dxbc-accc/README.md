# dxbc-accc

Classifies D3D shader containers (`DXBC` with `DXIL`, legacy `SHDR`/`SHEX`, and `ACCC` chunks carrying BetterXen's synthetic `FSPX` table) and exposes the C ABI in [`abi/include/bx_shader_bridge.h`](../../../abi/include/bx_shader_bridge.h): `bx_shader_inspect`, `bx_shader_prepare_for_d3dmetal` and `bx_shader_free_buffer`.

## Guest access: `bx_shader_bridge.dll` (#445)

The shims that see shader bytecode (`d3d12_xs.dll`) are x86-64 Windows PE32+ DLLs running inside CrossOver. They cannot load the arm64 macOS library that `cargo build` produces on the host. The bridge reaches guests as a **Windows build of this same crate**, with no second implementation:

- **Build:** `cargo build --target x86_64-pc-windows-gnu` turns the crate's `cdylib` into a PE32+ DLL, published as `bx_shader_bridge.dll`. It exports exactly the three header functions and imports only system DLLs that Wine provides (`KERNEL32`, `ntdll`, the UCRT `api-ms-win-crt-*` sets, `api-ms-win-core-synch-l1-2-0`, `bcryptprimitives`, `USERENV`, `WS2_32`). It needs no MinGW runtime DLL.
- **Callers:** a shim loads it with `LoadLibraryW` and `GetProcAddress`, so a missing bridge is a logged condition, not a load failure of the shim.
- **Why not a host tool over captured logs:** the M2 gate needs the decision at `Create*PipelineState` time inside the guest. That means passing DXIL through and rejecting ACCC-only containers. An offline host tool could count shaders but could not act on them.

### Build and check

```sh
rustup target add x86_64-pc-windows-gnu      # once
shader-transpiler/crates/dxbc-accc/guest/build.sh
target/debug/bx-cli run --prefix /tmp/bx-shader --shims target/bx-shader-bridge \
    --exe target/bx-shader-bridge/check_bridge.exe
```

`guest/build.sh` writes `bx_shader_bridge.dll` and `check_bridge.exe` to `target/bx-shader-bridge/`. It fails if the DLL exports anything other than the three entry points or imports a MinGW runtime DLL.

[`guest/check_bridge.c`](guest/check_bridge.c) is a C guest program built against the frozen header. It checks the struct layout with `_Static_assert` and sends eight containers through the DLL:
- a real DXC SM 6.6 compute blob (from [`d3dmetal-harness`](../d3dmetal-harness/README.md));
- synthetic DXIL pixel SM 6.0 with `RTS0`;
- `ACCC` with an `FSPX` v1 stage section, and `ACCC` with an `FSPX` v0 stub;
- `DXIL` + `ACCC`;
- legacy `SHEX`;
- two malformed blobs.

The program prints one `BXSHADER` line per container and a count line, and checks:
- **Classification:** kind, stage, shader model, chunk sizes, root signature and digest.
- **Prepare:** DXIL is copied byte for byte; ACCC-only and legacy are `BX_SHADER_ERR_UNSUPPORTED`; malformed input is `BX_SHADER_ERR_INVALID_BLOB`, with output cleared.
- **Free and argument errors:** freeing returned buffers, and null arguments at the DLL boundary.

Recorded run (one Apple M5 Pro, macOS 27.2.1, CrossOver 26.3 `26.3.0.39832`, both a fresh prefix and a D3DMetal bottle):

```text
BXSHADER counts dxil=2 accc_only=2 dual=1 legacy=1 unknown=0 invalid=2
PASS: check_bridge (8 containers through bx_shader_bridge.dll)
```

This is a synthetic, single-host result. It is not a measurement of any title's shader corpus, and not Xbox hardware.

### Not done here (other owners)

- **`abi/defs/bx_shader_bridge.def` (`[ABI]`):** the frozen export list for the DLL. Linking with it would also set the export directory's internal name, which is `dxbc_accc.dll` today. Wine loads by file name, so this does not affect loading.
- **Staging in `scripts/build-shims.sh` (Track A):** building the DLL there and verifying it with the other shims, so `build-shims/bin` ships it.
- **Calling it from `d3d12_xs` at PSO creation, and JSONL telemetry (#466):** today `bx_shader_prepare_for_d3dmetal` reports an ACCC-only container on the guest's stderr (`bx-shader event=prepare status=unsupported …`), not through `bx_log_emit`.
