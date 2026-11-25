# Independent review of the upstream registry and reuse matrix

Issue #363. Retrieved 2026-11-25 (all GitHub and web reads between about 20:20 and 20:40 UTC). The reviewer did not write the [upstream registry](upstream-registry.md) or the [component reuse matrix](component-reuse-matrix.md).

## Method and limits

The facts below were collected from primary sources first, starting from the project names and pins only, and compared with the registry afterwards:

- **License**: the license file(s) at the pinned commit, read in full or up to the grant, plus what GitHub's license detection reports for the repository and at the pin.
- **Pin**: whether the pinned commit exists, its committer date, and whether it was the default-branch head at retrieval (GitHub compare API).
- **Role and platform statements**: the README at the pinned commit.
- **CrossOver**: the [CodeWeavers source page](https://www.codeweavers.com/crossover/source).

Everything here is **evidence class D** (public project material). Nothing was installed, built or run, and this is not legal advice. No upstream code was copied. Per-file notices inside Wine, DXMT and DXVK were not reviewed; that remains an open item in the matrix.

Verdicts: **Confirmed** means the primary source agrees. **Corrected** means the primary source contradicts or materially adds to the entry, and the registry or matrix was updated in the same change. **Unknown** means the entry cannot be checked from public sources.

## Registry rows

| Project | Pin check | License at pin | Statements | Verdict |
| --- | --- | --- | --- | --- |
| CrossOver | n/a | n/a | The [source page](https://www.codeweavers.com/crossover/source) offers "FOSS Code for CrossOver 26.3.0", recommends each project's own home page, and lists Wine, Vkd3d (source.winehq.org), DXVK, MoltenVK, FAudio, SDL, LLVM, GMP and others. D3DMetal and Rosetta 2 are not listed. **DXMT is not listed either.** | Confirmed; corrected to note that DXMT is absent from the list |
| Wine | [`a79470449d03`](https://github.com/wine-mirror/wine/commit/a79470449d0388ed3b5ff7975a4cefd6a3d80a2a) exists, committed 2026-11-24, still head | [`LICENSE`](https://github.com/wine-mirror/wine/blob/a79470449d0388ed3b5ff7975a4cefd6a3d80a2a/LICENSE): LGPL "version 2.1 of the License, or (at your option) any later version"; `COPYING.LIB` is LGPL 2.1. GitHub: NOASSERTION. | Not reviewed further | Corrected: **LGPL-2.1-or-later**, not LGPL 2.1 only |
| DXMT | [`faa73e4a0686`](https://github.com/3Shain/dxmt/commit/faa73e4a0686e432ce5d3bb99a02b5363d76885c) exists, committed 2026-11-24, still head | [`LICENSE`](https://github.com/3Shain/dxmt/blob/faa73e4a0686e432ce5d3bb99a02b5363d76885c/LICENSE): LGPL 2.1 or any later version, "Copyright (c) 2023-2026 Feifan He for CodeWeavers". [`LICENSE.OLD`](https://github.com/3Shain/dxmt/blob/faa73e4a0686e432ce5d3bb99a02b5363d76885c/LICENSE.OLD): releases **up to v0.80 were MIT**. GitHub: NOASSERTION. | README: Metal-based D3D11/D3D10 layer for macOS using Wine; status on the project wiki. D3D12 not claimed. | Confirmed; corrected to record the earlier MIT releases |
| DXVK | [`57b2a7c1e87b`](https://github.com/doitsujin/dxvk/commit/57b2a7c1e87b6c9666b730ab700a18bea82fa1ca) exists, committed 2026-11-25, still head | `LICENSE`: zlib/libpng. GitHub: Zlib. | README: "running 3D applications on Linux using Wine"; no macOS statement. Listed on the CodeWeavers page. | Confirmed |
| VKD3D-Proton | [`71cae091b065`](https://github.com/HansKristian-Work/vkd3d-proton/commit/71cae091b06571e51446cf2a937f0b04c237c04d) exists, committed 2026-11-25, still head | [`COPYING`](https://github.com/HansKristian-Work/vkd3d-proton/blob/71cae091b06571e51446cf2a937f0b04c237c04d/COPYING): LGPL "version 2.1 ... or (at your option) any later version"; `LICENSE` is the LGPL 2.1 text. GitHub: LGPL-2.1 for the repository, NOASSERTION for the file at the pin. | README lists Vulkan 1.3 among hard requirements. | Corrected: **LGPL-2.1-or-later**; the Vulkan 1.3 statement is confirmed |
| MoltenVK | [`5189f8de86ec`](https://github.com/KhronosGroup/MoltenVK/commit/5189f8de86ec365860f63bf1da736815cfaea5cd) exists, committed 2026-10-13, still head | `LICENSE`: Apache License 2.0. GitHub: Apache-2.0. | Repository description: Vulkan Portability implementation layering "a subset" of Vulkan over Metal. Last push 2026-10-13. Listed on the CodeWeavers page. | Confirmed |
| FEX | [`3ee25399232a`](https://github.com/FEX-Emu/FEX/commit/3ee25399232a6431c77c6314c437ea523ec29d0c) exists, committed 2026-11-25, still head | `LICENSE`: MIT. GitHub: MIT. | Readme: runs x86 applications "on ARM64 Linux devices"; tested distributions are all Linux; no macOS host statement. | Confirmed |
| VIctory | [`13f91caee25f`](https://github.com/Marvisak/VIctory/commit/13f91caee25f2fdf2c062be5f7a27f058e51cbca) exists, committed 2026-11-25, still head | `LICENSE`: GPL version 3 text. GitHub: GPL-3.0. The registry's pointer to a local, untracked file (`experiments/victory-native/licenses.md`) cannot be checked: the file is not in the repository. | README: runs one commercial title's Xbox Series X\|S build on Windows from its game files; not playable. | Description confirmed; the per-file-notice claim is **unknown** |
| VIctory-macOS | [`485b230ef596`](https://github.com/pragyaangaur/VIctory-macOS/commit/485b230ef596898cdd390cd5d9205b76e4289b82) exists, committed 2026-11-25, still head; GitHub reports `fork: false`, no parent | `LICENSE`: GPL version 3 text. GitHub: GPL-3.0. | README: built on VIctory; stack is Rosetta 2, CrossOver Wine (built from CodeWeavers' published source) and D3DMetal; reports 701 of 711 tests passed on an M4; says it runs the title "from a copy of the game that you own". Test results are self-reported. | Confirmed |
| PortWatch VI | [`ed00263cc0c9`](https://github.com/4lwayz/portwatch-vi/commit/ed00263cc0c95ebce7e67de48b72184a11c98956) exists, committed 2026-11-25 19:48 UTC; six newer commits the same day (head at retrieval `c5b38ebd28e3`) | **No license file** at the pin (repository root holds only `README.md`; no license file anywhere in the tree). GitHub reports no license (the license API returns 404), not NOASSERTION. | README: tracks efforts to port "the leaked Xbox Series build" of the title; scores are "editorial estimates", self-reported claims are labelled, and candidates can be moved to an `ignored` list. Site `portwatch-vi.xyz` answered HTTP 200. | Corrected: no license detected and no license file (default copyright applies); the pin was head when reviewed but the repository changes several times a day |

## Additional candidates

| Project | Pin check | License at pin | Statements | Verdict |
| --- | --- | --- | --- | --- |
| llvm-mingw | [`903f4cf7faa2`](https://github.com/mstorsjo/llvm-mingw/commit/903f4cf7faa24e30c8e0c4615f8ce61a0f3d80f8) exists, committed 2026-11-24, still head | [`LICENSE.txt`](https://github.com/mstorsjo/llvm-mingw/blob/903f4cf7faa24e30c8e0c4615f8ce61a0f3d80f8/LICENSE.txt): the project's own scripts and wrappers are **ISC**; the built toolchain is under each component's license, "primarily LLVM's Apache 2 license with LLVM exceptions". GitHub: NOASSERTION. | README: cross compilers come in x86_64 and aarch64 versions built on Ubuntu. However, the latest release [20261117](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261117) (published 2026-11-18) includes `llvm-mingw-20261117-ucrt-macos-universal.tar.xz`. | Corrected: license resolved; a prebuilt macOS universal toolchain is published (not downloaded or run here, so its behaviour on Apple Silicon is still unknown). Relevant to #364. |
| Sikarugir | [`5446b7d18072`](https://github.com/Sikarugir-App/Sikarugir/commit/5446b7d18072301aa36c5b108d113708e033e724) exists, committed 2026-11-19, still head | No repository license file; GitHub reports none. The README states that `Configure.app` (a modified `Wineskin.app`) and other listed components are LGPL-2.1, and separately lists components that are not. The tree holds copies of Apple's D3DMetal `License.pdf` for versions 1.1, 2.0, 2.1, 3.0 and 4.0. | README: "successor to Wineskin", supports macOS 14.6 or later, Apple Silicon requires Rosetta 2; D3DMetal is described as closed source with a restrictive license. | Confirmed "not reported" for GitHub detection; corrected to add the README's per-component LGPL-2.1 statement and the D3DMetal license copies, which are third-party copies, not Apple's own download (see #354) |

## Reuse matrix decisions

| Need / candidate | Decision under review | Finding | Verdict |
| --- | --- | --- | --- |
| Windows runtime APIs / CrossOver | Adopt as external dependency | Strategic decision (ADR 0005), not a fact to re-derive. Its stated conditions (proprietary, do not bundle) agree with the source page. | Confirmed |
| Windows runtime APIs / upstream Wine | Reference; source may be consulted | License is LGPL-2.1-or-later, not 2.1 only. The decision does not change. | Corrected (license wording) |
| x86-64 on ARM64 / Rosetta 2 | Permitted where Apple supports it | Not re-derived here (#348). The row cited #349, which is closed and merged into #348. | Corrected (issue reference) |
| x86-64 on ARM64 / FEX | Not applicable now | Readme targets ARM64 Linux only. | Confirmed |
| Direct3D 12 to Metal / D3DMetal | Evaluate as external | Absent from the CodeWeavers FOSS list (confirmed). Terms are #354's scope. The row cited #349 (merged into #348). | Confirmed; issue reference corrected |
| Direct3D 11/10 to Metal / DXMT | Evaluate | LGPL-2.1-or-later confirmed. New facts: DXMT is not on CodeWeavers' FOSS list although its copyright line names CodeWeavers, and releases up to v0.80 were MIT. Neither changes the decision; whether CrossOver ships DXMT and where its source is offered is for #348 and #354. | Confirmed with added conditions |
| Direct3D to Vulkan / DXVK | Evaluate only together with MoltenVK | zlib/libpng confirmed; README states Linux. | Confirmed |
| Direct3D 12 to Vulkan / VKD3D-Proton | Evaluate; blocked on Vulkan 1.3 | Vulkan 1.3 requirement confirmed. Its license is LGPL-2.1-or-later (the matrix row states none). | Confirmed |
| Vulkan on Metal / MoltenVK | Evaluate | Apache-2.0 confirmed; "subset of Vulkan" confirmed. | Confirmed |
| Shader compiler, runtime / VIctory, VIctory-macOS | Rejected for reuse | Both target one commercial title's Xbox Series build. VIctory-macOS says users supply a copy they own; PortWatch describes the efforts as targeting a leaked build. The sources disagree, so authorization is **not established**, which is what the decision rests on. No wrong fact found. | Confirmed |
| Project discovery / PortWatch VI | Reference only | Editorial tracker per its README. No license file, so nothing could be imported anyway. | Confirmed |
| BetterXen native implementation; methodology; build around CrossOver | Preserved / Reuse / Build later | Internal planning decisions consistent with ADR 0005 and the roadmap; no upstream fact involved. | Confirmed (policy, not re-derived) |

## Provenance decision

The decision to exclude VIctory-family projects is confirmed: their GPL licenses cover their own source, and whether the material they were built for is authorized is disputed between sources (see the matrix row above). The registry's statement that a prior local review found per-file notices cannot be checked and should not be relied on until the notes are committed.

## Decisions to reopen

None. Every correction above changes wording, licenses or references; no adoption decision rested on a fact found to be wrong. The llvm-mingw macOS release is new input for #364, and the absence of DXMT from CodeWeavers' FOSS list is new input for #348 and #354.
