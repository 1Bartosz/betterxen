# D3DMetal and Apple GPU limits for a shader translator

Constraints that any translator emitting shaders for D3DMetal on Apple Silicon has to
handle, collected for the M2 shader-format gate and the M3 shader pipeline (#453).
Writing the translator is out of scope.

Each constraint states where its evidence comes from:

- **Fixture** — `tests/conformance/test_d3dmetal_gpu_limits`, an original synthetic
  D3D12 compute fixture that checks results against values computed on the CPU. Results
  are under [Fixture results](#fixture-results). A local Apple Silicon run of an original
  fixture, by the implementer: class A for the stated host only, not independently
  reproduced.
- **Reported** — notes from local VIctory-macOS-based builds (GPL-3.0, listed in
  [upstream-registry.md](upstream-registry.md)) running a Scarlett title, posted on #453.
  These are observations of a commercial title on one host. They are not BetterXen
  measurements, no code or shader from that project is adopted, and they need a
  synthetic reproduction before being used as evidence.

Nothing here is an Xbox hardware result, and nothing generalizes beyond the D3DMetal
versions named.

## 1. Console shaders arrive as RDNA 2 machine code

*Reported.* The title tested ships its shaders as native RDNA 2 code, with no DXIL to
fall back on. Running it needed a full ISA translator, which points the M2 gate towards
translation (Branch D2) rather than a DXIL fast path. Inspecting real corpora remains
#445 / M2.

## 2. `WaveSize(64)` does not run; wave64 programs need splitting

Apple GPUs run 32-lane SIMD groups. A program written for a 64-lane wave has to be split
into two 32-lane halves that agree wherever the original wave acted as one.

*Fixture:* `wave.control` (no `WaveSize`) and `wave.wavesize64` (`[WaveSize(64)]`, SM 6.6)
record the wave width each thread ran with. On D3DMetal 4.0b3 the device reports
`WaveLaneCountMin = WaveLaneCountMax = 32`. The control runs at width 32. The wave64
pipeline is **created without error but never runs**: its output buffer keeps the
sentinel. A translator therefore cannot rely on pipeline creation failing. It has to emit
32-lane code itself.

*Reported:* operations that need care when the two halves must agree:

- wave reductions, including 16-bit ones;
- `v_readfirstlane` and `v_readlane`;
- the 64-bit lane mask (`exec`, `vcc`) read as data;
- `v_permlanex16`;
- scalar registers spilled into vector lanes (`v_writelane` / `v_readlane`) and later
  joined as data;
- group barriers. A barrier is only safe where both halves provably reach it.

What made most refused programs translate exactly in those builds:

- **Uniformity dataflow.** A value differs between halves when it derives from a vector
  register input, a narrowed `exec`, a lane-numbered 64-bit constant,
  `mbcnt`/permlane/DPP, an LDS atomic or a clock. A barrier is placed only where every
  branch before it is on uniform values.
- **Loops on `SCC` only.** A loop whose every conditional branch is on `SCC` runs alike in
  both halves, so a barrier inside it is safe.
- **Commuting LDS atomics.** Atomics that commute (for example adds in a loop) need no
  ordering among themselves. They need ordering only after plain LDS writes, and reads
  after either.
- **Approximate fallback.** Where exact agreement cannot be proven, each half can run as
  its own wave. An LDS sync in a block that not every lane reaches must then be a memory
  fence without the group wait. A full barrier there hangs the group.

In those builds, LDS programs refused in halves fell from 21 to 1 and dropped passes from
about 246 to 28 per frame, with visibly restored effects (exposure, mips, reductions,
interior lighting). That supports the suspected link between refused programs and
missing effects. The programs still refused are a lane mask spilled through
`v_writelane`, a lane mask read as data where it was never tracked as a mask, and a wave
reduction across both halves.

## 3. 64-bit image atomics

*Fixture:* `atomic64.image` runs SM 6.6 64-bit `InterlockedAdd` (low halves carrying
into the high half) and `InterlockedMax` (decided by the high half) on a
`RWTexture2D<uint64_t>` over `DXGI_FORMAT_R32G32_UINT`. `atomic64.buffer` runs the same
operations on a raw buffer as a control, and the fixture prints the device's
`AtomicInt64OnTypedResourceSupported` and `AtomicInt64OnDescriptorHeapResourceSupported`.
This separates "typed 64-bit atomics are wrong" from "64-bit atomics are wrong
everywhere". A plain 64-bit store next to the atomics shows whether each kernel ran at
all.

On D3DMetal 4.0b3:

- The device reports both `AtomicInt64On*` features as supported, and both pipelines are
  created without error.
- Neither kernel runs. The atomic results and the plain-store marker all read back as their
  initial zeros, on the texture and on the raw buffer alike.
- A throwaway variant with the atomics removed but the markers kept wrote 8/8 markers, so
  the readback path is sound and the skip comes from the atomics.

In short, a kernel that uses SM 6.6 64-bit atomics is silently skipped there. This is
wider than #453 assumed: raw-buffer atomics are affected too, not only images. It
probably explains why d3dmetal-native's `d3d12-atomic64-test` read back zeros, as the
[d3dmetal-harness](../../shader-transpiler/crates/d3dmetal-harness/README.md)
evaluation recorded on D3DMetal 3.0.

### Lock-based emulation

*Fixture:* `lock64.cap64` and `lock64.cap1024` emulate both operations with a per-texel
lock word (`InterlockedCompareExchange` 0→1, update the two 32-bit halves,
`DeviceMemoryBarrier`, release with `InterlockedExchange`), 32 contending threads per
texel and a bounded number of tries. A thread that runs out of tries counts a drop
instead of spinning on. The 1024-try run is required to drop nothing and to match the
CPU reference exactly. The 64-try run is reported. On D3DMetal 4.0b3:

- **1024 tries:** nothing dropped and 16/16 values exact in all three runs, with about
  14,000–16,000 tries in all.
- **64 tries:** 20–28 of 512 updates dropped.

So a low cap trades a hang for silently lost updates: with 32 contending threads, 2^6 is
already too low. Apple's shader converter prints `Warning: Undefined: Shader might rely on
forward progress/persistent thread` for this kernel, although the loop is written so that
it does not.

The loop shape matters. The critical section sits **inside** the retry loop, so a lane
that wins finishes it in the same iteration and no lane waits for another lane to leave
a loop. In the classic shape (`while (!acquire()) {}` then the critical section), the
winning lane can be parked at the loop's reconvergence point behind the lanes still
spinning, and a GPU without independent forward progress for each thread never finishes.
The fixture does not run that shape, because it could hang the queue.

*Reported* (pixel shaders, not covered by the fixture):

- **Helper lanes must not take part in the lock.** Their atomics are dropped, so they never
  win and spin to the cap on every turn. That caused a hard queue hang: one submission
  never completed, and macOS logged no GPU timeout. Apple's shader converter warns that
  such a shader relies on forward progress. Excluding helper lanes fixed it.
- **The retry cap.** At 2^16 tries the GPU watchdog fired and the queue stopped accepting
  work, while 2^6 and 2^10 ran without hangs. A cap of 2^22 was not adopted because a stall
  that long looks exactly like a hang. Whether the 2^16 hang came from the cap itself or
  from helper lanes spinning to it has not been separated. The fixture above shows that 2^6
  can drop updates under contention, so "ran without hangs" does not mean "correct".

## 4. Mesh output size: 32 KiB

*Reported.* Metal refuses mesh pipelines whose per-vertex outputs exceed 32,768 bytes
(`Total mesh size (35840) exceeds ... 32768`). Under Wine, submitting a refused pipeline
anyway can hang the queue, so a translator must refuse or shrink it itself.

- **Layout.** Metal lays a vertex out as clip and cull distances, then position, then the
  outputs in register order, each aligned to its own size. What counts is the aligned
  layout, not the packed component count.
- **Shrinking it.** Two changes took these errors from 37 to 9:
  - ordering the outputs within each register row for the least padding;
  - linking a mesh shader to its pixel shader, so only the outputs the pixel shader reads
    are kept and the rows are numbered anew on both sides.
- **The remaining 9** suggest that D3DMetal's count depends on more than the outputs.

## 5. Apple shader compiler crash on early-merged output stores

*Reported.* `MTLCompilerService` crashed in LLVM register allocation, seen as
`XPC_ERROR_CONNECTION_INTERRUPTED`. The crash reports in
`~/Library/Logs/DiagnosticReports/MTLCompilerService-*.ips` name `AGCModule<MeshProgramKey>`.
It hit about half of the mesh pipelines whose pixel shader writes several render targets,
and it reproduced offline outside Wine.

- **Cause.** The shader converter merged an output row's stores at the first store, so
  values computed later in the program were used before their definition. That is invalid
  IR: `air-opt`'s verifier reports "does not dominate".
- **Fix.** Emitting each output row's stores only after all of its values are computed
  took the crashes from about 220 per run to 0.

## 6. No zero-copy heaps

*Reported.* D3DMetal 4.0b3 answers `ID3D12Device3::OpenExistingHeapFromAddress` with
"Not implemented". Guest memory therefore cannot be placed straight into a D3D12 heap, and
resource contents must be copied or aliased by other means.

## Fixture results

A local run by the implementer on 2026-11-26. Class A for this host only; not
independently reproduced, not CrossOver 26.3, and not an Xbox hardware result.

| | |
| --- | --- |
| Host | Apple M3 Pro, macOS 27.2.1 (26B131), Rosetta 2 |
| Wine | CodeWeavers Wine 11.0 built from CodeWeavers' published source; CrossOver itself was not installed |
| D3DMetal | 4.0b3 from Apple's Game Porting Toolkit (x86_64; SHA-256 of the binary starts with `b16aab78127d`), user-supplied |
| Shaders | DXC 1.9.2609 via `shaders/compile.sh`; the same DXC reproduces the d3dmetal-harness `fill_cs66` container byte for byte (SHA-256 `7df11b63…`) |
| Command | `ROSETTA_ADVERTISE_AVX=1 WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=b" wine build-shims/bin/test_d3dmetal_gpu_limits.exe` in a fresh prefix |
| Runs | 3, identical outcomes; exit 0, `SUMMARY: 4 required checks, 0 failed` each time |

| Check | Outcome |
| --- | --- |
| `device.shader_model` | 6.6 |
| `wave.reported` | `WaveOps=1`, lane count min 32, max 32 |
| `wave.control` | ran at width 32, lane layout consistent (required: pass) |
| `wave.wavesize64` | created but not run (output still sentinel) |
| `atomic64.reported` | typed and descriptor-heap 64-bit atomics reported as supported |
| `atomic64.buffer` | not run: marker 0/8, atomic values 16/16 still zero |
| `atomic64.image` | not run: marker 0/8, atomic values 16/16 still zero |
| `lock64.cap64` | 20, 28 and 20 of 512 updates dropped (values not compared) |
| `lock64.cap1024` | 0 of 512 dropped, 16/16 values exact (required: pass) |

Still to do for the issue's validation requirement: an independent run on CrossOver 26.3
(D3DMetal 3.0) by another contributor.

## Regenerating the shaders

`tests/conformance/test_d3dmetal_gpu_limits/shaders/compile.sh` regenerates the committed
headers with Microsoft's DXC release v1.9.2609 (`dxc_2026_09_29.zip`, SHA-256
`ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1`, the same package the
d3dmetal-harness records), running `bin/x64/dxc.exe` under a Wine loader.
