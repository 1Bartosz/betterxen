# Synthetic PE32+ Conformance Tests (`tests/conformance/`)

Every directory matching `test_*/CMakeLists.txt` is **auto-discovered** by `tests/conformance/CMakeLists.txt` when running `scripts/build-shims.sh`.

## Adding a New Test (Zero Shared-File Edits)

1. Create `tests/conformance/test_<shim>/CMakeLists.txt` and `main.cpp`.
2. Build via `scripts/build-shims.sh` (outputs `build-shims/bin/test_<shim>.exe`).
3. Run live under CrossOver + Rosetta 2:
   ```sh
   target/debug/bx-cli run --prefix /tmp/bx-test --shims build-shims/bin --log /tmp/bx-test.jsonl --exe build-shims/bin/test_<shim>.exe
   ```
