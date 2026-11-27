# BetterXen Frozen ABI Contracts (`abi/`)

This directory holds the **only shared interface boundary** across BetterXen's six parallel development tracks.

## Rules for Contributors

1. **ABI-First Changes:** If one track needs a new cross-DLL helper, config flag, or export symbol, open a small, focused `[ABI]` pull request touching only `abi/`.
2. **Zero File Collisions:** Never edit another track's implementation directory in an `[ABI]` PR. Once the `[ABI]` PR merges to `main`, both provider and consumer tracks implement in parallel.
3. **Versioned Contracts:** Increment `BX_ABI_VERSION` in `abi/include/bx_config.h` on any breaking struct or signature change.
4. **Canonical `.def` Exports:** Every DLL under `shims/` links directly against its corresponding `abi/defs/<name>.def`. CI verifies the built PE32+ export table matches these `.def` files. One entry is not under `shims/`: `bx_shader_bridge.def` is the export list of `bx_shader_bridge.dll`, the x86-64 Windows build of `shader-transpiler/crates/dxbc-accc` (#445). `scripts/build-shims.sh` links it into that build and verifies it with the shims (#484).
