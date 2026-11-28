# Synthetic PE32+ Conformance Tests (`tests/conformance/`)

Every directory matching `test_*/CMakeLists.txt` is **auto-discovered** by `tests/conformance/CMakeLists.txt` when running `scripts/build-shims.sh`.

## Adding a New Test (Zero Shared-File Edits)

1. Create `tests/conformance/test_<shim>/CMakeLists.txt` and `main.cpp`.
2. Build via `scripts/build-shims.sh` (outputs `build-shims/bin/test_<shim>.exe`).
3. Run live under CrossOver + Rosetta 2:
   ```sh
   target/debug/bx-cli run --prefix /tmp/bx-test --shims build-shims/bin --log /tmp/bx-test.jsonl --exe build-shims/bin/test_<shim>.exe
   ```

## Exit Status: Pass, Fail, and Unavailable

`bx-cli run` normally returns the guest's exit status. A timeout returns 124, a signal
returns 128 plus its number, and a cleanup failure raises an otherwise successful status
to at least 1. Fixtures that cannot reach a conclusion on the host report **exit 77**
and print `SKIP: <reason>` on stdout; for example
`test_d3dmetal_gpu_limits` skips when `d3d12.dll`, device creation, or shader model 6.6 is
unavailable.

A skip is **not** a pass and **not** a failure. AGENTS.md §5 forbids counting a skipped test
as a pass, so record the three outcomes separately:

```sh
target/debug/bx-cli run --prefix /tmp/bx-test --shims build-shims/bin \
    --log /tmp/bx-test.jsonl --exe build-shims/bin/test_<shim>.exe
status=$?
case $status in
    0)   echo "PASS: test_<shim>" ;;
    77)  echo "UNAVAILABLE (skip): test_<shim> — see the SKIP line above" ;;
    *)   echo "FAIL: test_<shim> (exit $status)" ;;
esac
```

`bx-cli` does not itself reinterpret 77 (it is the guest's own status, and other fixtures use
their own conventions), so always read the status before reporting a run. For a fixture that
adopts this protocol, printing a `SKIP:` line without exit 77, or exiting 77 without one, is a
defect. Older per-check skip conventions, such as `test_d3d12_xs`, are separate: its `SKIP:`
lines are informational and the fixture exits according to `g_failures`.
