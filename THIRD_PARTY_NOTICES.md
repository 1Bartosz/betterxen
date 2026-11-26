# Third-party notices

The active BetterXen Rust workspace (`Cargo.toml` / `Cargo.lock`) currently has **zero external crates.io dependencies** and is original `GPL-3.0-or-later` source.

## External Host Tools & Runtime Dependencies (Never Bundled)

| Component | Role | Licensing & Provenance |
| --- | --- | --- |
| CrossOver / Wine | External macOS Windows compatibility runtime | Proprietary product with FOSS subcomponents; externally installed by the user and never bundled. See [docs/research/crossover-licensing.md](docs/research/crossover-licensing.md). |
| Apple D3DMetal & Rosetta 2 | External Direct3D-to-Metal and x86-64 translation | Proprietary Apple components under Apple's license terms; externally installed and never bundled. |
| `mingw-w64` & CMake | Developer cross-compilation toolchain for x86-64 PE32+ shims | Installed externally via Homebrew (`brew install mingw-w64 cmake`) for building `shims/` and `tests/conformance/`. |

Historical third-party notices for the retired native-first workspace (including AMD RDNA 2 ISA metadata, `bitflags`, and `objc2-*`) remain preserved at Git tag `native-first-final` and are summarized in [docs/licenses.md](docs/licenses.md).
