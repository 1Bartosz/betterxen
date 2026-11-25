# CrossOver licensing and redistribution boundaries

Research note for the hybrid direction. It records what the public license
texts say about the components a CrossOver-based BetterXen would rely on, and
which documents are still missing. It is not legal advice.

Reviewed 2026-11-25 against public sources only. No CrossOver installation, no
Apple license text and no proprietary component was downloaded for this note.

## Distribution model

BetterXen does not bundle, redistribute or modify CrossOver, D3DMetal, Rosetta 2
or any of the open-source components listed below. The intended model is an
externally installed CrossOver that BetterXen invokes as a separate program, with
all third-party components supplied by their own distributors. This note records
licenses so that any later decision to ship, link or copy a component has a
starting point; it does not approve one.

## Components

| Component | Version or revision read | License read | Source | Redistribution in BetterXen | Notes |
| --- | --- | --- | --- | --- | --- |
| BetterXen | current `main` | GPL-3.0-or-later | [`LICENSE`](../../LICENSE) | The project itself | Compatibility baseline for every row below |
| CrossOver (product) | 26.3.0, per CodeWeavers' source page | Proprietary product terms; the EULA text was not located (see unknowns) | [CodeWeavers FOSS source page](https://www.codeweavers.com/crossover/source) | Not bundled; externally installed | The source page publishes the FOSS source tarball for 26.3.0 and lists the open-source projects below. It is not the product EULA |
| Wine | `master`, read 2026-11-25 | LGPL-2.1-or-later | [`LICENSE`](https://raw.githubusercontent.com/wine-mirror/wine/master/LICENSE) | Not bundled or linked today | "either version 2.1 of the License, or (at your option) any later version" |
| DXMT | `main`, read 2026-11-25 | LGPL-2.1-or-later | [`LICENSE`](https://raw.githubusercontent.com/3Shain/dxmt/main/LICENSE) | Not bundled or linked today | Copyright 2023–2026 Feifan He for CodeWeavers; same "or later" clause |
| DXVK | `master`, read 2026-11-25 | zlib | [`LICENSE`](https://raw.githubusercontent.com/doitsujin/dxvk/master/LICENSE) | Not bundled or linked today | Permission to use, alter and redistribute freely; the notice must be retained |
| VKD3D-Proton | `master`, read 2026-11-25 | LGPL-2.1-or-later | [`LICENSE`](https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/LICENSE), source headers | Not bundled or linked today | The source headers carry the "either version 2.1 … or any later version" grant; the `LICENSE` file is the LGPL 2.1 text |
| MoltenVK | `main`, read 2026-11-25 | Apache-2.0 | [`LICENSE`](https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/LICENSE) | Not bundled or linked today | Apache-2.0 is compatible with GPL-3.0 but not GPL-2.0; `NOTICE` obligations apply if it is ever distributed |
| Apple Game Porting Toolkit repository | `main`, read 2026-11-25 | Apache-2.0 | [`LICENSE`](https://raw.githubusercontent.com/apple/game-porting-toolkit/main/LICENSE) | Not bundled | This is the repository's license for its porting resources. It is not the license for the separately distributed D3DMetal framework |
| D3DMetal | Game Porting Toolkit 4.0, reported | Unconfirmed: a third-party copy of the 4.0 `License.pdf` was read outside this repository; Apple's own download was not read | — | User-supplied only; never bundled | Reported (evidence class D, not confirmed against Apple's copy): a personal licence to install, use and test for developing, testing or evaluating games on Apple hardware, distribution solely for non-commercial purposes, no use on non-Apple hardware, Framework and Redistributables not separable, and no reverse engineering (sections 2A, 2C, 2D). The missing document is Apple's own copy in the Game Porting Toolkit download. Do not treat the repository's Apache-2.0 license as D3DMetal's |
| Rosetta 2 | macOS feature | Unknown: Apple's applicable software license was not read | [Apple Rosetta documentation](https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment) | External OS component; not bundled | The documentation describes the translation environment; it is not the license grant |

## Compatibility with GPL-3.0-or-later

LGPL-2.1-or-later, zlib and Apache-2.0 are generally compatible with a
GPL-3.0-or-later work, subject to each license's notice and source-availability
conditions. That compatibility matters only if BetterXen links to or copies the
component. Invoking an externally installed CrossOver as a separate program does
not make its components part of BetterXen's distributed work, and this note does
not decide otherwise.

Two rows cannot be reasoned about from the licenses available here: the CrossOver
product terms and D3DMetal. Neither may be bundled, linked or copied until its
actual license text has been read and recorded.

GPL-2.0-only components cannot be combined into this GPL-3.0-or-later workspace.
A comment on this issue identified MacBox and ScarlettMac as GPL-2.0; they are
not part of the component list above and this note does not rely on them.

## Unknowns and the exact missing documents

- **CrossOver product EULA.** The CodeWeavers legal pages checked on 2026-11-25
  did not return the EULA. Record the document URL and the exact clauses that
  cover redistribution, reverse engineering and bundling before relying on any
  product-level permission.
- **D3DMetal license.** It ships with the Game Porting Toolkit download; that
  download was not made. A third-party copy of the 4.0 `License.pdf` was
  reported to this issue with a section-level summary, but it is not Apple's
  copy and stays evidence class D. The `apple/game-porting-toolkit` repository is
  Apache-2.0 and does not establish the framework's terms. Confirm the summary
  against Apple's own copy before relying on it; until then D3DMetal is
  user-supplied and never bundled.
- **Rosetta 2 terms.** Apple's license text was not read. Rosetta 2 stays an
  external macOS component and is never redistributed by BetterXen.
- **Exact revisions.** Upstream files were read at `master`/`main` on
  2026-11-25, not at the revisions CodeWeavers ships in 26.3.0. Pin the shipped
  revisions before any reuse or notice work.
- **Linking versus invocation.** Which components, if any, BetterXen would link
  against rather than invoke is an architecture decision that has not been made
  here.

## Sources

- CodeWeavers, "Free and Open Source Software Code for CrossOver", listing the
  26.3.0 source tarball and the open-source projects:
  <https://www.codeweavers.com/crossover/source> (accessed 2026-11-25).
- Wine `LICENSE`, wine-mirror/wine at `master`, accessed 2026-11-25.
- DXMT `LICENSE`, 3Shain/dxmt at `main`, accessed 2026-11-25.
- DXVK `LICENSE`, doitsujin/dxvk at `master`, accessed 2026-11-25.
- VKD3D-Proton `LICENSE` and source headers, HansKristian-Work/vkd3d-proton at
  `master`, accessed 2026-11-25.
- MoltenVK `LICENSE`, KhronosGroup/MoltenVK at `main`, accessed 2026-11-25.
- Apple Game Porting Toolkit repository `LICENSE`, apple/game-porting-toolkit at
  `main`, accessed 2026-11-25. Repository license only; it is not the D3DMetal
  license.
- Apple, "About the Rosetta translation environment", accessed 2026-11-25.
  Documentation only, not the license text.
