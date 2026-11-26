# D3D12 swapchain references under D3DMetal

An observation to check before M2 builds presentation on `IDXGISwapChain3`. One Apple M4,
macOS 27, CrossOver 26.3.0 (26.3.0.39832), 2026-11-26. Not independently reproduced, and not an
Xbox hardware result.

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
