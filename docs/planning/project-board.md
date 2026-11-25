# Project board setup: BetterXen Development

Status: **not configured.** The automation token used for this transition lacks the GitHub `project` scope, and GitHub's API cannot create views. Tracked by [#346](https://github.com/MacosPorters/betterxen/issues/346).

1. `gh auth refresh -s project`, then create an organization project named "BetterXen Development".
2. Fields (single select unless noted): Track (hybrid, shared, native), Phase (H0 to H7), Status (Backlog, Ready, In Progress, In Review, Validation, Blocked, Done), Priority (P0, P1, P2), Area (text), Complexity (S, M, L), Validation (none, local, independent), Blocked (text). Use the built-in Milestone, Assignees and Labels instead of duplicate fields.
3. Add all open issues and pull requests with `gh project item-add`.
4. Views (web UI): Active Development (`-label:"track: native"`), Hybrid Roadmap (group by Phase), Critical Path (`label:"status: blocked"` plus those blocking H1), Ready for Development (`label:"status: ready"`), In Progress, Review and Validation, Performance (Area), Deferred Native Research (`label:"track: native"`; clearly secondary).
5. Do not import old priority labels; priorities must be re-set by maintainers.
