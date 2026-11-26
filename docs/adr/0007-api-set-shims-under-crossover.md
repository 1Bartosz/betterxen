# ADR 0007: API-set win32_bridge shims are not reached under CrossOver

Status: **Accepted** (2026-11-26, issue #437). Independently verified on CrossOver 26.0 and CrossOver 26.3 on Apple Silicon.

## Context

`shims/win32_bridge/` builds six DLLs named after Windows API sets:
`api-ms-win-core-psm-appnotify-l1-1-0`, `api-ms-win-core-psm-appnotify-l1-1-1` and four `ext-ms-win-rtcore-ntuser-*` forwarders to `user32.dll`. A guest that imports those names statically is expected to load the BetterXen files placed next to the EXE. #437 reports that under CrossOver 26.3 it does not: Wine answers the names itself, even with `WINEDLLOVERRIDES=<name>=n,b` and the shim files in the EXE's directory.

## Evidence

`tests/conformance/test_apiset_resolution` imports one export from each of the six DLLs through the PE import directory, then reports for every name the module that owns the name and the module the loader bound the import address table slot to.

Run on an Apple M-series Mac, macOS 27.2.1, Rosetta 2, **CrossOver 26.0 (`26.0.0.39794`)**, bare prefix, with the override list from `bx-cli` (`SHIM_DLL_OVERRIDES`) and `build-shims/bin` as the EXE directory:

| Import name | Served by | Module that owns the name | Module the import is bound to |
| --- | --- | --- | --- |
| `api-ms-win-core-psm-appnotify-l1-1-0` | host | `c:\windows\system32\twinapi.appcore.dll` | same |
| `api-ms-win-core-psm-appnotify-l1-1-1` | host | `c:\windows\system32\twinapi.appcore.dll` | same |
| `ext-ms-win-rtcore-ntuser-message-l1-1-0` | host | `c:\windows\system32\user32.dll` | same |
| `ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0` | host | `c:\windows\system32\user32.dll` | same |
| `ext-ms-win-rtcore-ntuser-window-l1-1-0` | host | `c:\windows\system32\user32.dll` | same |
| `ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0` | host | `c:\windows\system32\user32.dll` | same |

`WINEDEBUG=+loaddll` shows `Loaded` records for `user32.dll` and `twinapi.appcore.dll` (both builtin) and none for any of the six shim files. `--expect shim` fails 0/6, so the fixture does distinguish the two outcomes.

This behaviour was independently reproduced on **CrossOver 26.3** on Apple Silicon (`bx-cli run` with the shim directory and direct loader runs with `WINEDEBUG=+loaddll`), yielding the exact same 6/6 host resolution (`twinapi.appcore.dll` and `user32.dll`, both builtin, with no `Loaded` records for the six shim files) and `--expect shim` exiting 1. The internal Wine mechanism is not claimed here; this records observed loader behaviour.

## Decision

Rely on Wine's built-ins. Under CrossOver the six API-set shims are not part of the supported path: the host answers window and message calls through `user32.dll` and PSM registration through `twinapi.appcore.dll`.

- The six DLLs stay in the tree, built and export-checked, and are still exercised by explicit full-path loads (#431).
- No attempt is made to make them win name resolution. That would depend on undocumented Wine API-set behaviour (#350) and, within this issue's scope, on patching or redistributing CrossOver or Wine components, which is out of scope.
- Offline PSM behaviour that a title needs must live somewhere a static import actually reaches. This ADR does not choose that place.

## Consequences

- `test_apiset_resolution` records the current behaviour (`--expect host`) and fails when it changes. If a CrossOver build starts loading the shims, run it with `--expect shim` and revisit this ADR.
- `test_integration_boot` continues to report `shim_backing` as pending for these six names under CrossOver.
- Documentation describes the bridges as Windows-only stubs that CrossOver does not load for static imports.

## Not claimed

Nothing here shows Xbox title compatibility, behaviour on Xbox hardware, or behaviour on CrossOver builds outside 26.0 and 26.3. Evidence class: local Apple Silicon runs.
