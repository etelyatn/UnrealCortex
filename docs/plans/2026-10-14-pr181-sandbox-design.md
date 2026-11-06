# PR181 Sandbox integration design

## Approved scope

Retain every behavior from PR181 (`9ee2b9915a7933458c19c0122d4791ed67e91dc3`), correct the source-confirmed legacy widget identity regression, independently exercise the missing acceptance paths, then publish and merge when acceptance permits. User approved this bounded correction/review/delivery route in the current conversation. No functionality expansion or replacement architecture.

## Integration

Current target is `8bead43c120d3e428a7ba33f3000611a71f7b038`. Merge target into the submitted history, preserving attribution and current-main tree bounds/command contracts. Combine the fixture semantically: use main's capability document and PR181's `editor.inject_mouse` declaration; regenerate fallback. Owning repository is UnrealCortex. Normal local branch only; no worktrees, resets, cleaning, stashing, force pushes, or unrelated parent/gitlink edits. Publication uses an additive update to the original same-repository PR branch when remote head is unchanged.

## Identity correction

Empty identity maps represent legacy initialization, not a map with no pre-existing variables. Before adding the first registered widget identity, populate existing source-widget and animation identities with the same path-derived deterministic GUIDs used by the native compiler. Populated maps retain their existing values. Both widget addition and animation creation must follow this invariant using the existing UMG utility owner if a shared helper removes duplicated initialization. All mutation remains in the existing transaction after Blueprint Modify. No new public API or persistence format.

## Preserved behaviors

- Exact-class closed orphan admission, including entryless explicit inventories; no broad deletion, retained consumers, graph-owner admission, or weakened approval.
- Exact node/pin identity, graph enumeration and reciprocal link enumeration on rollback, required compile/readback and explicit save.
- Symbolic byte-backed enum inspection and same-family ordinary Object Blueprint cleanup.
- Whole-tree guard fingerprint distinct from property-binding guards; GUID preservation and source identity collision refusal.
- Native SlateColor tracks and strict per-section RestoreState/KeepState, RestoreState by default.
- PIE request-generation correlation, coalesced owned cancellation ticker, genuine cancellation/input cleanup.
- Correct existing mouse descriptor; no dispatch changes or new tool.

## Acceptance

1. Legacy Designer widget addition: existing root/child/animation deterministic identities exist before compilation; compile emits no warnings/errors/ensures; populated identities survive; undo/redo restores identity and hierarchy consistently.
2. Retirement: inject failure after an actual target compile; verify recovery compile, exact native graph/pin/link ordering, original reviewed token applicability and no saved disk change. No precompile fault substituted for this phase.
3. Animation: create a real runtime UserWidget from a compiled authored Blueprint; play native SlateColor tracks; observe consumer color during playback and after completion for RestoreState/KeepState. Exercise undo/redo and retained independent tracks.
4. PIE: real start/stop/new-start requests, genuine rejected/cancelled start, and successor continuous input must observe callback/state/token/value ownership, not manually assigned state alone.
5. Supported Sandbox build and affected native suites, isolated offline MCP tests, actual routed smoke and toolkit contract audit. Warnings are classified from raw logs, not hidden.
6. Independent exact-candidate review; expected remote head protected before additive publication; merge and final remote state confirmed. Preserve unrelated work and non-task branches.

## Non-goals

No generic node retirement, generalized identity framework, engine shims, retries, telemetry, new tool, game logic, fixture suppression, or unrequested repository cleanup.
