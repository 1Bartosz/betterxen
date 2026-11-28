# Synthetic PE32+ Conformance Tests (`tests/conformance/`)

Every directory matching `test_*/CMakeLists.txt` is **auto-discovered** by `tests/conformance/CMakeLists.txt` when running `scripts/build-shims.sh`.

## Adding a New Test (Zero Shared-File Edits)

1. Create `tests/conformance/test_<shim>/CMakeLists.txt` and `main.cpp`.
2. Build via `scripts/build-shims.sh` (outputs `build-shims/bin/test_<shim>.exe`).
3. Run live under CrossOver + Rosetta 2:
   ```sh
   target/debug/bx-cli run --prefix /tmp/bx-test --shims build-shims/bin --log /tmp/bx-test.jsonl --exe build-shims/bin/test_<shim>.exe
   ```

## M2 First-Frame Fixture

The M2 fixture requires both the placed-resource and PresentX headers. Its CMake target fails
configuration if either provider contract is missing; the fixture must not be silently skipped.
Run it with a D3DMetal bottle:

```sh
target/debug/bx-cli run --bottle <d3dmetal_bottle> --backend d3dmetal \
  --shims build-shims/bin --log /tmp/bx-m2-first-frame.jsonl \
  --exe build-shims/bin/test_m2_first_frame.exe
```

Replace `<d3dmetal_bottle>` with the installed CrossOver bottle name.

To verify that the pixel comparison detects a mismatch, set the injection variable in the
launcher environment and opt into environment inheritance. `env -i` keeps this test-only
variable and the listed runtime essentials from exposing unrelated host environment values to
the guest:

```sh
env -i HOME="$HOME" USER="$USER" LOGNAME="$LOGNAME" TMPDIR=/tmp LANG=C \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  BX_M2_FIRST_FRAME_INJECT_PIXEL_MISMATCH=1 \
  target/debug/bx-cli run --inherit-env --bottle <d3dmetal_bottle> --backend d3dmetal \
  --shims build-shims/bin --log /tmp/bx-m2-first-frame-negative.jsonl \
  --exe build-shims/bin/test_m2_first_frame.exe
```

Replace `<d3dmetal_bottle>` with the installed CrossOver bottle name.

The negative control should exit 1 with a `pixel_readback` checkpoint identifying pixel
`(0,0)` as mismatched.
