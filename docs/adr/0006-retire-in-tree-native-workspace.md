# ADR 0006: Retire in-tree native-first workspace to tag `native-first-final`

Status: **Accepted.** Owner directive amending [ADR 0005](0005-crossover-hybrid-direction.md).

## Context

ADR 0005 pivoted BetterXen's active engineering track to a CrossOver-based hybrid architecture for Apple Silicon while keeping the paused native-first implementation (`crates/`, `app/`, `benchmarks/`, `experiments/`, `fixtures/`, `fuzz/`, legacy scripts, legacy CI, and legacy native docs) in-tree.

Keeping two unrelated Rust workspaces, ~20 native-only scripts, dual CI workflows, and hundreds of paused native files in-tree creates maintenance drag, broken script assumptions, and onboarding noise for contributors and coding agents working on the 6-track hybrid architecture.

## Decision

- **Retire the in-tree native-first codebase, scripts, workflows, and native-only documentation from `main`.**
- **Preserve the complete native-first history permanently at Git tag `native-first-final`.** Nothing is lost; any future work that needs an archived component (for example, RDNA 2 decoder tables if Milestone M2 activates Branch D2 shader transpilation) can selectively inspect or retrieve it from `native-first-final`.
- Keep only active hybrid source, frozen cross-track contracts (`abi/`), native ARM64 tooling (`launcher/`), x86-64 PE32+ shims (`shims/`), shader classification/transpilation (`shader-transpiler/`), auto-discovered conformance binaries (`tests/conformance/`), repository hygiene checks (`scripts/audit/docs_hygiene.py`), and active governance/hybrid documentation (`docs/`).

## Consequences

- **Positive:** Single Cargo workspace at repo root; single build/CI surface (`hybrid-ci.yml` + `hygiene.yml`); zero broken legacy scripts; clean 6-track ownership for 5–8+ concurrent contributors and coding agents.
- **Negative:** Native-only open pull requests targeting the retired `crates/` layout are superseded by the hybrid track; revisiting native-first execution requires checking out `native-first-final` or filing a focused hybrid issue.
