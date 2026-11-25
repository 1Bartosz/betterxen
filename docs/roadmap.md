# BetterXen roadmap

**This is the single authoritative active roadmap.** GitHub Issues are the task backlog, the GitHub project board tracks operational state, and this file explains strategy, phases, dependencies and completion criteria. If another document disagrees, this one governs, except [ADR 0005](adr/0005-crossover-hybrid-direction.md) for the decision itself.

## Direction

BetterXen's active direction is a **CrossOver-based hybrid architecture for Apple Silicon**. CrossOver is the intended compatibility foundation and is treated as an **externally installed dependency**. BetterXen's own work is the layer around it: integration, configuration, diagnostics, reproducible testing, benchmarking and measured optimization. The independent native implementation (x86-64 decoder, ARM64 translator, runtime, gfx1030 shader compiler, Metal backend) is **preserved but paused**; see the [deferred native roadmap](research/native-roadmap.md).

This is the intended architecture, **not a verified Xbox compatibility stack**. Nothing in this repository demonstrates that CrossOver, Rosetta 2 or D3DMetal run any Xbox title, and CrossOver is not assumed to provide Xbox runtime compatibility. All workloads must be independently authored, publicly licensed or otherwise authorized.

```
BetterXen: integration, configuration, diagnostics, benchmarking, optimization
        |
CrossOver (external): Wine-based runtime APIs | Rosetta 2 CPU translation | D3DMetal / DXMT graphics
        |
Apple Silicon: ARM64, Metal, unified memory

Independent native implementation: paused, preserved for research and component reuse
```

Priorities, in order: practical compatibility, functional integrated execution, rendering correctness, stability, performance, Apple Silicon-specific optimization, long-term independence. Reuse before reinvention; measure before optimizing; evidence before claims.

## Engineering rules

The former rules forbidding Rosetta, Wine, CrossOver and MoltenVK belonged to the native-first plan. [AGENTS.md](../AGENTS.md) (with `CLAUDE.md` as a symlink to it) was rewritten to match this roadmap and [ADR 0005](adr/0005-crossover-hybrid-direction.md): CrossOver, Wine-based infrastructure, Rosetta 2 (where supported), D3DMetal (subject to license) and open-source components are permitted; MoltenVK, DXMT, DXVK, VKD3D-Proton and FEX may be evaluated; proprietary components are never bundled; leaked or unauthorized material stays excluded. This resolves [#345](https://github.com/MacosPorters/betterxen/issues/345).

## How progress is reported

Phase status is one of: Not started, Researching, In development, Blocked, In validation, Verified complete, Retired. A phase is Verified complete only with the evidence its exit conditions name. Progress is **not** derived from commits, lines of code, closed issues, instruction counts or passing isolated fixtures. No phase is currently Verified complete. Percentages, where used for task counts, are never compatibility or readiness figures.

## Planning hierarchy

Project, Phase, GitHub milestone, engineering deliverable, GitHub issue, pull request, validation evidence. Milestones H0 to H7 exist on GitHub with the text below.

## Phases

### H0 — CrossOver Architecture Transition

**Status:** In development

**Objective:** Make CrossOver the single authoritative active compatibility foundation across the repository and GitHub planning surfaces.

**Scope:** README, architecture docs, ADR 0005, rewritten engineering rules, roadmap, upstream research registry, technical plans, labels, milestones, issue and PR templates, issue triage, project board, preservation of the native implementation.

**Dependencies:** None.

**Deliverables:** Updated README and docs; ADR 0005; docs/roadmap.md; deferred docs/research/native-roadmap.md; rebuilt issues and milestones; project board; preserved native code.

**Validation:** Repository link/terminology check; GitHub consistency check.

**Exit conditions:** Repository, documentation, issues, milestones and board consistently state the CrossOver-based direction. The project board requires a token with the project scope and is not yet configured.

**Known risks:** Project board cannot be configured without the GitHub project token scope (#346). The engineering rules conflict was resolved by rewriting AGENTS.md (#345).

### H1 — CrossOver Research and Baseline

**Status:** Not started

**Objective:** Establish what the installed CrossOver version actually provides and a reproducible baseline, without assuming it provides Xbox runtime compatibility.

**Scope:** CrossOver architecture, supported macOS versions, Apple Silicon support, relationship to upstream Wine, CPU translation mechanism, graphics backends (D3DMetal, DXMT and others), supported Direct3D versions, configuration interfaces, documented command-line interfaces, bottle management, diagnostics and profiling options, licensing and redistribution, supported integration mechanisms, long-term maintenance. Open-source and proprietary components distinguished.

**Dependencies:** Engineering rules rewritten (#345, done). CrossOver is an externally installed dependency until its license says otherwise; nothing is bundled.

**Deliverables:** CrossOver capability and integration-boundary report; licensing and dependency assessment; reproducible environment; baselines of CPU, GPU, memory and stability on authorized workloads.

**Validation:** Every claim cites vendor or upstream documentation, or a recorded observation on the installed version. Unknown stays unknown. Equivalent workloads, settings, hardware and software versions.

**Exit conditions:** Capabilities and integration boundaries are documented for the installed version; environment is reproducible by another contributor; baselines recorded; BetterXen vs CrossOver responsibilities proposed.

**Known risks:** CrossOver is proprietary: bundling, modifying or redistributing may be prohibited; integration interfaces may be undocumented; Xbox titles may not run at all.

### H2 — BetterXen Integration Foundation

**Status:** Not started

**Objective:** Build the BetterXen layer around CrossOver: integration boundaries, configuration, diagnostics, reproducible test workflows and host integration.

**Scope:** Environment and configuration management, diagnostics, test workflows, verified host integration, regression testing. Only what CrossOver does not already provide adequately.

**Dependencies:** H1.

**Deliverables:** Boundary document; configuration and environment management where justified; diagnostic infrastructure; reproducible test workflows; verified host integration; regression tests.

**Validation:** Independent contributor reproduces the workflows; no placeholder components.

**Exit conditions:** Integration foundation builds, tests and reproduces on a second setup.

**Known risks:** Reliance on undocumented or changing CrossOver behavior.

### H3 — Compatibility Validation

**Status:** Not started

**Objective:** Evaluate authorized workloads and document verified behavior and limitations.

**Scope:** Authorized workloads, documented limitations, runtime behavior, graphics correctness, input and interaction, reproducible failures, integrated validation criteria.

**Dependencies:** H2.

**Deliverables:** Validation criteria; per-workload results; reproducible failure reports; verified runtime, graphics and input results.

**Validation:** Independent verification of rendering and behavior. Startup or a screenshot alone is not proof.

**Exit conditions:** Selected workloads show verified integrated behavior under documented criteria; failures reproducible.

**Known risks:** Failures may originate in CrossOver or its components outside project control.

### H4 — Graphics and Performance Engineering

**Status:** Not started

**Objective:** Evaluate graphics backends and optimize only demonstrated bottlenecks.

**Scope:** Supported backends, CPU/GPU utilization, unified memory, frame-time consistency, synchronization, shader compilation and caching. Metrics: average FPS, 1% low, frame-time percentiles, CPU and GPU utilization, unified memory, memory pressure, translation overhead where measurable, shader compile latency, startup, thermals, long-duration stability. Reference machine: M4 Mac, 24 GB.

**Dependencies:** H3; measurement protocol.

**Deliverables:** Backend comparison; profiling reports; bottleneck list; before/after results; regression tests.

**Validation:** Equivalent workloads, rendering settings, hardware and versions; correctness and rendering equivalence verified.

**Exit conditions:** Improvements measurable, reproducible and without loss of correctness or stability. No improvement is promised before measurement.

**Known risks:** Many components are closed and not modifiable; single reference machine.

### H5 — Stability and Hardware Scaling

**Status:** Not started

**Objective:** Evaluate Apple Silicon configurations, memory pressure, thermals and long-duration stability.

**Scope:** Available Apple Silicon configurations, memory pressure, thermal behavior, long-duration tests, hardware documentation, regression coverage.

**Dependencies:** H3; hardware access.

**Deliverables:** Hardware compatibility documentation; memory-pressure and thermal reports; long-duration results; expanded regression coverage.

**Validation:** Each documented configuration backed by a test run on it.

**Exit conditions:** Tested configurations documented; untested hardware never claimed.

**Known risks:** Only one reference machine currently available.

### H6 — Hybrid Architecture Maturity

**Status:** Not started

**Objective:** Maintainable integration infrastructure and contributor workflows.

**Scope:** Contributor workflows, verified-capability documentation, reproducible performance results, complexity reduction, long-term dependency risk (CrossOver, Rosetta 2, D3DMetal).

**Dependencies:** H3 to H5 evidence.

**Deliverables:** Maintainable infrastructure; contributor documentation; verified capability list; reproducible results; dependency risk assessment.

**Validation:** Contributors other than the authors maintain the workflow.

**Exit conditions:** The architecture shows demonstrated value and the team can maintain it.

**Known risks:** Vendor decisions (CrossOver licensing, Rosetta lifetime) are outside project control.

### H7 — Future Native Reassessment (deferred)

**Status:** Not started

**Objective:** Deferred. Reassess independent native components only on an explicit maintainer decision or when one offers a demonstrated benefit to the active architecture.

**Scope:** See docs/research/native-roadmap.md.

**Dependencies:** Explicit maintainer decision.

**Deliverables:** Reassessment record.

**Validation:** Maintainer decision recorded in an ADR.

**Exit conditions:** A recorded decision to continue, resume specific native work, or archive components.

**Known risks:** Premature reinvention of mature infrastructure.

## Phase tracking issues

| Phase | Tracker |
| --- | --- |
| H0 | [#344](https://github.com/MacosPorters/betterxen/issues/344) |
| H1 | [#347](https://github.com/MacosPorters/betterxen/issues/347) |
| H2 | [#352](https://github.com/MacosPorters/betterxen/issues/352) |

Trackers for H3 to H7 are created when their prerequisites are close, to avoid placeholder tasks.

## Research already done

The [upstream registry](research/upstream-registry.md) and [component reuse matrix](research/component-reuse-matrix.md) record what public sources say about CrossOver, Wine, DXMT, DXVK, VKD3D-Proton, MoltenVK, FEX, VIctory-macOS, VIctory and PortWatch (evidence class D, nothing executed). Findings that shape the plan: FEX targets ARM64 Linux and is not a macOS option on that evidence; D3DMetal and Rosetta 2 are not in CodeWeavers' list of FOSS components, so they are external and non-redistributable until their terms are verified; VKD3D-Proton needs a Vulkan 1.3 provider that MoltenVK may not offer; VIctory-family projects are built for a specific commercial game's Xbox Series build and are excluded from reuse on provenance grounds. Independent verification is [#363](https://github.com/MacosPorters/betterxen/issues/363).

## Critical path

Priority order, with parallelism marked:

1. **Engineering policy transition**: done in this branch ([#345](https://github.com/MacosPorters/betterxen/issues/345), closed once the branch is reviewed).
2. **Parallel desk research** (no execution needed): CrossOver architecture [#348](https://github.com/MacosPorters/betterxen/issues/348); Rosetta 2, D3DMetal and Direct3D coverage [#349](https://github.com/MacosPorters/betterxen/issues/349); supported integration interfaces [#350](https://github.com/MacosPorters/betterxen/issues/350); licensing and redistribution [#354](https://github.com/MacosPorters/betterxen/issues/354); independent check of the upstream registry [#363](https://github.com/MacosPorters/betterxen/issues/363).
3. **Authorized workload selection and measurement protocol** [#353](https://github.com/MacosPorters/betterxen/issues/353), in parallel with step 2.
4. **Reproducible environment and baselines**: [#303](https://github.com/MacosPorters/betterxen/issues/303) capability receipt and [#355](https://github.com/MacosPorters/betterxen/issues/355) baseline. Needs #353.
5. **Responsibility boundary ADR** [#351](https://github.com/MacosPorters/betterxen/issues/351): needs steps 2 and 4.
6. **Implementation tasks justified by the ADR**: created under H2 ([#352](https://github.com/MacosPorters/betterxen/issues/352)); shared items already triaged there are [#257](https://github.com/MacosPorters/betterxen/issues/257) (output-path guards). Later: [#145](https://github.com/MacosPorters/betterxen/issues/145) frame-time capture (H4), [#204](https://github.com/MacosPorters/betterxen/issues/204) memory pressure (H5).

Hard dependencies are GitHub blocked-by relationships; everything else is preferred order. The shortest legitimate path to a verified baseline is steps 2 (the CrossOver parts), 3 and 4.

## Immediate work

| Issue | Track | Phase | Priority | Depends on | Expertise | Deliverable |
| --- | --- | --- | --- | --- | --- | --- |
| [#348](https://github.com/MacosPorters/betterxen/issues/348) | hybrid | H1 | p1 | none | research | CrossOver architecture, versions, Wine relationship, sourced |
| [#349](https://github.com/MacosPorters/betterxen/issues/349) | hybrid | H1 | p1 | none | research | Rosetta 2, D3DMetal, Direct3D coverage, sourced |
| [#350](https://github.com/MacosPorters/betterxen/issues/350) | hybrid | H1 | p1 | none | research | Supported integration interfaces |
| [#354](https://github.com/MacosPorters/betterxen/issues/354) | hybrid | H1 | p1 | none | licensing | Licensing and redistribution assessment |
| [#353](https://github.com/MacosPorters/betterxen/issues/353) | shared | H1 | p1 | none | test engineering | Workload set and measurement protocol |
| [#363](https://github.com/MacosPorters/betterxen/issues/363) | hybrid | H1 | p2 | none | reviewer | Independent check of registry and matrix |
| [#303](https://github.com/MacosPorters/betterxen/issues/303) | shared | H1 | p1 | none (design); run after #353 | test engineering | Host and dependency capability receipt |
| [#346](https://github.com/MacosPorters/betterxen/issues/346) | shared | H0 | p2 | project token scope | repo admin | Configured project board |

Parallelizable now: all of the above. Next cycle: [#355](https://github.com/MacosPorters/betterxen/issues/355) baseline, then [#351](https://github.com/MacosPorters/betterxen/issues/351). Later: H2 tasks from the ADR, H3 validation, H4 performance. No deadlines are set for research and none are inferred from contributor availability.

## Existing issues

The native backlog was reviewed individually. 84 native issues were closed as consolidated into seven "[Deferred native research]" umbrella issues ([#356](https://github.com/MacosPorters/betterxen/issues/356) CPU, [#357](https://github.com/MacosPorters/betterxen/issues/357) DBT performance, [#358](https://github.com/MacosPorters/betterxen/issues/358) GPU, [#359](https://github.com/MacosPorters/betterxen/issues/359) integration workloads, [#360](https://github.com/MacosPorters/betterxen/issues/360) runtime, [#361](https://github.com/MacosPorters/betterxen/issues/361) compiled launch and app, [#362](https://github.com/MacosPorters/betterxen/issues/362) roadmap research); issue text is preserved and each can be reopened. 27 native issues with discussion, an assignee or a linked open pull request stay open as `track: native` and `status: deferred`, without milestone or priority. The eight former shared-infrastructure issues were triaged: [#303](https://github.com/MacosPorters/betterxen/issues/303) to H1, [#257](https://github.com/MacosPorters/betterxen/issues/257) to H2, [#145](https://github.com/MacosPorters/betterxen/issues/145) to H4 and [#204](https://github.com/MacosPorters/betterxen/issues/204) to H5 (rewritten for the hybrid stack); [#173](https://github.com/MacosPorters/betterxen/issues/173), [#174](https://github.com/MacosPorters/betterxen/issues/174) and [#219](https://github.com/MacosPorters/betterxen/issues/219) to H6 as useful but not currently necessary; #158 closed as obsolete and consolidated into #357. Nothing was deleted.

The native-first plan is archived at [archive/development-phases-native-first.md](archive/development-phases-native-first.md); retired milestones are in [archive/native-first-milestones.json](archive/native-first-milestones.json). Open pull requests that target native work remain open for their authors and maintainers to decide.
