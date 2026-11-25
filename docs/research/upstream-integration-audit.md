# Upstream integration audit (2026-11-25)

Companion to the [upstream registry](upstream-registry.md) and [component reuse matrix](component-reuse-matrix.md); those two hold the canonical rows, this file holds the audit evidence behind the new ones. Evidence class D unless stated: public repository metadata, file trees and files read through the GitHub API, plus one Apple license read as text. **No upstream script, installer, binary or build hook was executed and no upstream code was copied.** Not legal advice. CrossOver is **not installed** on the reference machine (Apple M4, 24 GB, macOS 27.0 build 26A428), so nothing below was validated against CrossOver.

## Contributor-reported progress (unverified)

skakwy (a BetterXen contributor) maintains [skakwy/victory-mac](https://github.com/skakwy/victory-mac) (GPL-3.0 per GitHub; head `main` pushed 2026-11-25). Reports received: about 1 frame per second with a commercial game, and later "two rendered frames". Both are contributor statements. No hardware, macOS or CrossOver version, workload, instrumentation or raw data has been shared, and the repository's own README gives only build and run commands. Treat both as **unverified**; neither is a compatibility claim. The repository describes itself as a fork of VIctory, a project whose purpose is a leaked Xbox Series build of a commercial game (see the registry), so it is a discovery reference at documentation level only. Information worth requesting from skakwy is listed under "Contributor request" below.

## Projects reviewed

Pins are the default-branch head or the commit read on 2026-11-25. "Equivalent" is what BetterXen has or lacks. Status values: **implemented** (code and tests on this branch), **reference** (read, nothing taken), **rejected**.

| Project | URL | Pin | Licence (file read where stated) | Verified facts | BetterXen equivalent | Provenance | Classification | Status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| victory-mac | github.com/skakwy/victory-mac | not pinned (not reviewed past README and tree) | GPL-3.0 (GitHub) | Rust workspace with `mac/` build and run scripts for CrossOver, D3DMetal, Rosetta 2; README says "VIctory fork with a Mac build" | none needed | Built on a game-specific upstream whose inputs are unauthorized; file-level review not done | Technical reference only | reference |
| VIctory-macOS | github.com/pragyaangaur/VIctory-macOS | see registry | GPL-3.0 (GitHub) | Described as an attempt to run one commercial game | none needed | Same concern | Rejected for reuse | rejected |
| MacBox | github.com/aykhanium/MacBox | not pinned | **GPL-2.0** (GitHub) | README is candid: "not playable", "retail gameplay is unverified", test suite "known failures"; layers on XCL, an Xbox Series runtime for unpacked console executables | none | Game-specific; also **GPL-2.0-only is not combinable with GPL-3.0-or-later** (see decisions) | Rejected for reuse | rejected |
| ScarlettMac | github.com/paveledits/ScarlettMac | not pinned | GPL-2.0 (GitHub); also `LICENSE-silicon-rans`, `SOURCE-NOTICES.md` | Patch set and shell scripts for XCL on CrossOver; README states game screenshots and a game-specific status | none | Game-specific; mixed licences | Rejected for reuse | rejected |
| PortWatch VI | portwatch-vi.xyz | see registry | NOASSERTION | Editorial tracker | none | Not engineering evidence | Reference only | reference |
| ossover | github.com/hemkum/ossover | `5c11bd7be8da` | GPL-3.0-or-later (README, `ATTRIBUTION.md` lists per-file adaptations from Whisky and highball) | Swift package: `SysInfo` (macOS, chip, memory, Rosetta probe), `Bench/MetalHUDLog` (parser for `metal-HUD:` log lines), `Bench/Harness`, `Store/EngineProbe`, `Renderer/CrossOverBottleConfig`, `doctor` command. README: alpha, one game verified on one machine, "no benchmark numbers have been published". Its design was derived from reverse engineering a CrossOver 26.3.0 bundle | Host receipt, frame statistics (this branch) | Clean, attribution explicit. Reverse-engineered CrossOver internals are **not** a supported interface | Technical reference; ideas re-implemented independently | reference |
| Whisky | github.com/Whisky-App/Whisky | `fd5480a76b3e` | GPL-3.0-or-later (file header read: "version 3 ... or (at your option) any later version") | Archived 2025-05-11. `WhiskyKit`: bottle settings, Wine process control, PE parser, Rosetta 2 check | Rust host receipt covers the Rosetta check | Clear | Technical reference only (Swift; no reuse path) | reference |
| rum | github.com/lunguini/rum | `657461b194e1` | GPL-3.0 (GitHub) | Active Whisky fork: DXMT/DXVK setup, run history, graphics settings views | none needed | Fork of Whisky | Technical reference only | reference |
| Sikarugir | github.com/Sikarugir-App/Sikarugir | `5446b7d18072` | none reported | 23 files; hosts copies of Apple's Game Porting Toolkit documents (`D3DMetal/<ver>/License.pdf`) | none | Redistributes Apple documents; no licence of its own | Reference only; no reuse | reference |
| Sikarugir-foss-sources | github.com/Sikarugir-App/Sikarugir-foss-sources | not reviewed | LGPL-2.1 (GitHub) | Fork of Wineskin sources | none | Unreviewed | Reference only | reference |
| Mythic | github.com/MythicApp/Mythic | see registry | GPL-3.0 | Launcher using a custom GPTK implementation | none | Not reviewed | Reference only | reference |
| DXMT | github.com/3Shain/dxmt | see registry | LGPL-2.1-or-later (LICENSE read by prior pass); GitHub shows NOASSERTION | D3D11/10 on Metal under Wine | Selected by CrossOver as a backend | Clear | External dependency (via CrossOver) | reference |
| DXVK, VKD3D-Proton, MoltenVK | see registry | see registry | zlib, LGPL-2.1, Apache-2.0 | No change from registry | none | Clear | External (via CrossOver) | reference |
| Gcenx macOS Wine builds | github.com/Gcenx/macOS_Wine_builds | `72f8915f2d01` | none reported | Community Wine packages | none | Binary provenance unreviewed | Reference only | reference |
| llvm-mingw | github.com/mstorsjo/llvm-mingw | `903f4cf7faa2` | NOASSERTION per GitHub; LICENSE not yet read | ARM64-capable mingw toolchain | `toolchain` probe (this branch) finds it if installed | Clear | External dependency (candidate for #364) | reference |
| Wine | github.com/wine-mirror/wine | see registry | LGPL-2.1 | Upstream of CrossOver's runtime | none | Clear | Reference | reference |
| Apple Game Porting Toolkit licence | text of `D3DMetal/4.0/License.pdf`, copy hosted by Sikarugir | document read, 6 pages | Apple proprietary agreement | See next section | none | Third-party copy of an Apple document; the original download was not read | Proprietary; user-supplied only | reference |

## Apple Game Porting Toolkit licence (for #354)

Read from the 4.0 `License.pdf` copy in the Sikarugir repository (not from Apple's site; confirm against Apple's download before relying on it). Section 2A grants a limited, personal licence to install, internally use and test the software "for the sole purpose of developing, testing, or evaluating video games for use on Apple-branded products", and to distribute it "solely for non-commercial purposes". Section 2C forbids use on non-Apple hardware, says components may not be separated from the bundle except that the Framework and Redistributables may be distributed separately (still non-commercial only), and section 2D forbids reverse engineering. Consequence for BetterXen: D3DMetal stays a user-supplied external component and is never bundled or redistributed by this repository. Whether running unrelated Windows software through it fits "evaluating video games for use on Apple-branded products" is a question for a lawyer, not answered here.

## Decisions

1. **No external code was incorporated.** The useful general-purpose ideas (host receipt, frame statistics, toolchain probe) are small, and the closest implementations are Swift, so they were written independently in Rust from public system interfaces. ossover and Whisky are credited as technical references in this file; nothing is adapted.
2. **GPL-2.0-only projects (MacBox, ScarlettMac) cannot be combined into this GPL-3.0-or-later workspace** regardless of provenance. Both are also game-specific.
3. **Undocumented CrossOver internals are not a stable interface.** ossover reads `cxbottle.conf` and bundle internals derived from reverse engineering. The new CrossOver module reads only `Info.plist`, `lipo -archs` and the `defaults` keys CodeWeavers documents, and states so in its output.
4. **The Metal HUD log parser in ossover was not ported.** Its line format (`metal-HUD: …`) is documented only by that project's observation; it was not verified against Apple documentation or a real trace here. Frame statistics therefore take plain intervals. A HUD parser can be added when a real trace is captured (#145).

## Implemented on this branch

New crate `crates/hybrid` (`betterxen-hybrid`, no third-party dependencies, native arm64 verified with `lipo -archs`: `arm64`).

| Command | Purpose | Issue | Validated |
| --- | --- | --- | --- |
| `host` | macOS version and build, chip, memory, core split, whether an x86_64 binary executes (Rosetta), GPU and Metal family from `system_profiler` | #353 | Run on the M4 reference machine; unit tests parse a GPU fixture |
| `crossover` | Finds `CrossOver.app` in standard locations and by Spotlight bundle identifier; reads version, build, executable architectures, minimum macOS, and the documented `BottleDir` and `ManagedBottleDirs` preferences | #350, #355 | Unit test on a **synthetic** bundle only. On the reference machine it reports `installed: false`. It has never inspected a real CrossOver |
| `toolchain` | Lists mingw-w64 and llvm-mingw cross-compilers on `PATH` with version and target | #364 | Stub-compiler unit test; on the reference machine it found Homebrew `x86_64-w64-mingw32-gcc` 16.2.0 and `i686-w64-mingw32-gcc`. No ARM64 Windows target is installed. No program was compiled or run |
| `frames <file>` | Frame-time statistics: mean, FPS, nearest-rank p50/p95/p99/p99.9, 1 % and 0.1 % lows, stutter count | #145 | Hand-derived known-answer tests, rejection of non-positive or non-finite intervals |
| `receipt` | The above as one JSON document, `betterxen-hybrid-receipt/0` | #353 | Run once; no second contributor has reproduced it |

Not implemented: launching programs, bottle management, graphics-backend selection or detection, GPU or thermal sampling, a HUD parser, any rendering workload. None is justified yet by Phase 1 evidence.

## Contributor request (to send, not sent)

Ask skakwy only for shareable, general information: Mac model, macOS and CrossOver versions, how frame rate was measured and over what duration, whether the two frames were presented or merely rendered, and any general-purpose tooling and authorized test workloads he wrote himself. Do not ask for game files, traces or instructions for running the leaked build.

## Remaining blockers

CrossOver is not installed here, so criteria 2 and 6 of Phase 1 cannot progress on this machine; `docs/research/primary-source-verification-2026-11-25.md` lists documents still to read. The Rust toolchain default (`stable`, 1.94.1) is older than the workspace's 1.100; use `rustup run 1.100.0 cargo`.
