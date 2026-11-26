# D3D12 swapchain references under D3DMetal

An observation to check before M2 builds presentation on `IDXGISwapChain3`. One Apple M4,
macOS 27, CrossOver 26.3.0 (26.3.0.39832), 2026-11-26. See the independent Apple M2 Pro run below.

| | D3DMetal | Wine `vkd3d` path |
| --- | --- | --- |
| References on the swapchain right after creation | 2 | 1 |
| Count returned by the application's final `Release` | 1 | 0 |
| Swapchains still referenced after 12 create/release cycles | 12 | 0 |

The same program was used for both, and it submitted no GPU work of its own. Under D3DMetal
each retained swapchain also kept a device reference. The cause inside D3DMetal is unknown. A
program that recreates swapchains (for example on resize) should be expected to grow until this
is explained.

The probe is
[`lifetime.c` at `7c512af`](https://github.com/MacosPorters/betterxen/blob/7c512af/tests/conformance/test_d3d12_lifetime/lifetime.c)
on the branch of #425; it is not part of this branch. Tracked in #450.

## Independent run on Apple M2 Pro

On 2026-11-26, an Apple M2 Pro (16 GiB, macOS 27.3 build 26C5035r) reproduced
the swapchain retention in CrossOver 26.3.0.39832 with D3DMetal 3.0. The
same x86-64 probe under Rosetta 2 released all swapchains in CrossOver
Preview 27.0.0.41303. Each CrossOver build used a fresh Windows 11
64-bit bottle. The Direct3D 12 workload loaded the `apple_gptk` D3D12
and DXGI modules in both builds. The bundled framework in the Preview
build's `apple_gptk` directory reports version 4.0b3.

| Swapchain lifetime probe | Initial reference count | Application release returned | Witnesses retained |
| --- | ---: | ---: | ---: |
| 26.3 D3DMetal | 2 | 1 | 12 of 12 |
| 26.3 Wine D3D12 | 1 | 0 | 0 of 12 |
| Preview D3DMetal | 1 | 0 | 0 of 12 |
| Preview Wine D3D12 | 1 | 0 | 0 of 12 |

Destroying the window before releasing the swapchain and calling
`ResizeBuffers` before teardown each left 3 of 3 witnesses retained on
26.3 D3DMetal. Explicitly detaching the witness released the probe's
private data, while the swapchain's release still returned 1. The
deliberately retained witness control failed as expected. An extra
application-side `Release` would drop a reference the application
does not own.

The separate D3D12 integrated workload presented 600 frames and verified
eight readbacks with zero mismatches on both D3DMetal builds. It failed
only the shutdown gate on 26.3, where the final device release returned
2. Preview passed shutdown with a final device release of 0. These runs
used the direct host D3D12 path without BetterXen's D3D12 shim. The
comparison establishes behavior for these two CrossOver builds on this
host. The underlying ownership change is unknown.

Build the probe from the pinned source linked above with
`x86_64-w64-mingw32-clang -std=c11 -O2 -Wall -Wextra -Werror lifetime.c -o d3d12-lifetime.exe -ld3d12 -ldxgi -ldxguid -luser32`
(LLVM-MinGW 20261117 in this run). Run in a clean bottle using
`ROSETTA_ADVERTISE_AVX=1 CX_GRAPHICS_BACKEND=d3dmetal wine --bottle <bottle> <absolute-path-to-exe> --cycles 12`
and repeat with `CX_GRAPHICS_BACKEND=wined3d`. The two witness controls
use `--cycles 3 --detach-witness` with and without `--retain-app-reference`.

## Related Direct3D acceptance failures

The [integrated D3D12 workload](https://github.com/MacosPorters/betterxen/blob/7c512af/tests/conformance/test_d3d12_integrated/integrated12.c)
also reproduced the presentation slowdown on the Wine D3D12 path on
26.3. With `CX_GRAPHICS_BACKEND=wined3d` and a 600-frame target, the
workload logged approximately 2,000 ms inside `Present` on frames
12–38, then exited 3 after a 10-second GPU completion wait failed. The
previous M4 report saw the slowdown around frame 437. This run exited
before reaching normal teardown. Preview completed the same 600-frame
workload on the Wine path with eight verified readbacks, no mismatches
and clean shutdown. A 600-frame repeat on 26.3 passed with `--vsync`
enabled. A subsequent 600-frame repeat also passed with the flag off,
so the slowdown remains intermittent and the successful vsync run
does not establish a workaround.

The [integrated D3D11 workload](https://github.com/MacosPorters/betterxen/blob/7c512af/tests/conformance/test_d3d11_integrated/integrated.c)
completed the 20,000-frame soak on 26.3 with 46 verified readbacks,
zero mismatches and a clean shutdown. The intermittent GPU event
stall did not recur in this run. Preview also completed all 20,000
frames and all 46 readbacks, then failed the shutdown gate with one
device reference remaining. A separate 600-frame Preview run
reproduced that shutdown result.

The 26.3 keyboard check delivered five macOS events to the workload and
reached the expected offset (16, 32). The negative control delivered
two Up events and reached (0, -32), which the input gate rejected as
expected. These were isolated runs, so they do not reproduce the
intermittent failure observed inside the full acceptance suite.
