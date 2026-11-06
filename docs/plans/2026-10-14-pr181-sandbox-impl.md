# PR181 Sandbox Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: subagent-driven-development. Controller owns serialized builds/tests, integrated acceptance, review, publication and merge. Workers never publish or run overlapping checks.

**Goal:** Ship all reviewed PR181 behaviors with the legacy widget identity regression corrected and missing Sandbox effects independently exercised.

**Architecture:** Preserve the existing graph journal, UMG operations/native MovieScene tracks, Blueprint cleanup family checks and PIE state tracker. Use bounded corrections in existing owners; no new public contracts beyond the submitted changes.

**Tech Stack:** Unreal Engine 5.8, native C++ automation, Python MCP, uv/pytest, GitHub CLI.

**Spec:** `2026-10-14-pr181-sandbox-design.md`; approved bounded recommendations in the current conversation.

## Global Constraints

- All UObject access and mutation on the Game Thread.
- No domain-to-domain dependencies, game types, compatibility shims or implicit save.
- Preserve exact reviewed inventory/approval/readback/rollback guards.
- Preserve all pre-existing user dirt and branch histories; normal branches, no worktrees/reset/clean/stash/force push.
- Every maintainer production correction follows observed native RED then GREEN.
- No worker builds/tests/formatters mid-flight. Controller runs one build/test process at a time after worker edits settle.
- Retain contributor history/attribution. No silent scope reduction.

## Review Focus

- Empty-map widget addition must initialize existing animation identities too, not just attached widgets.
- Populated identity maps and undo/redo must not replace existing GUIDs.
- Retirement recovery must occur after target compile, not merely after removal.
- Animation success must mean the runtime consumer receives color/completion state, not just channels existing.
- Stale PIE cancellation must preserve the new session's real continuous input, while genuine cancellation terminates its pending callback.

## Task 1: Legacy identity correction

Files: `Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp`, `CortexUMGWidgetAnimationOps.cpp`, existing `CortexUMGUtils` private owner if sharing initialization; `Source/CortexUMG/Private/Tests/CortexUMGWidgetVariableTest.cpp` or separate module-unique regression file.

- [ ] Build a valid legacy UWidgetBlueprint with pre-existing root, child and UWidgetAnimation, an empty identity map, and a fresh package. Call the public `umg.add_widget` handler to add a child.
- [ ] Assert each pre-existing identity equals `FGuid::NewDeterministicGuid(Object->GetPathName())` before compile; fail/return before compilation if missing to avoid repeated ensure noise during RED. Assert populated-map identity preservation separately; exercise transaction undo/redo and hierarchy membership.
- [ ] Controller builds and runs the focused regression; capture the missing-identity assertion as RED.
- [ ] Populate all legacy source-widget/animation identities using native path-derived GUIDs before first registration, in the existing transaction; share only this responsibility if both callers need it. Preserve nonempty maps.
- [ ] Controller rebuilds/runs regression and UMG suite; zero unexpected diagnostics. Review correction and tests.

## Task 2: Retirement recovery acceptance

Exclusive file: `Source/CortexGraph/Private/Tests/CortexGraphMigrationRetireTest.cpp`. Existing fixture/fault controls remain authoritative; do not introduce new production injection APIs.

- [ ] Extend the ordinary closed-orphan fixture with a genuine post-target-compile readback failure using existing automation-only fault hooks.
- [ ] Snapshot exact node enumeration, GUID/pin signatures, defaults, reciprocal LinkedTo ordering, approved validation token and disk bytes. Dispatch the reviewed mutation with compile=true/save=false.
- [ ] Verify target compilation preceded the injected failure; verify recovery compilation and exact native state, then prove the original token can still authorize the same intent. Exercise omitted-entry inventory through recovery if fixture supports it without creating a second graph system.
- [ ] Controller executes exact retirement cases and Graph suite; source-backed review confirms the fault phase and assertions.

## Task 3: Native animation playback acceptance

Exclusive file: a module-unique `Source/CortexUMG/Private/Tests/CortexUMGAnimationPlaybackTest.cpp` if no existing consumer fixture suffices. Do not duplicate existing metadata tests.

- [ ] Reuse authored Blueprint/binding/track helpers and compile a real UserWidget class. Create a runtime widget with a TextBlock SlateColor property and an independent retained track.
- [ ] Author guarded color sections with RestoreState and KeepState through the public operation. Native playback/time advancement must drive actual color; assert original baseline vs final retained value after completion. No direct property assignment substituted for animation evaluation.
- [ ] Exercise completion-only replacement and undo/redo; verify independent track remains intact. Use deterministic frame boundaries and local fixture cleanup, never global GC.
- [ ] Controller runs rendered playback cases and affected animation/UMG suites; observe consumer values and raw diagnostics.

## Task 4: PIE live acceptance

Owner: controller; a scoped worker prepares a throwaway public-route driver using current TCP/connection APIs without running it while other checks are active. No manually assigned state used as engine proof.

- [ ] Launch a task-owned fresh Sandbox editor; bind by its project/PID/instance/port. Never stop user-owned editors.
- [ ] Real start -> stop -> new-window start; check callback success, Playing state and active session after deferred cancellation tick.
- [ ] Exercise a real pending request cancellation and assert its callback/state cleanup. Start a successor and inject continuous input; observe actual Enhanced Input value after correct world-tick boundaries and ensure stale cancellation cannot acquire/cancel successor ownership.
- [ ] Capture native mouse operation schema and corrected public discovery output. No pointer response is promoted to a genuine UI click claim.
- [ ] Remove throwaway drivers after receipts are retained.

## Task 5: Integrated evidence and delivery

- [ ] Run supported build, focused changed cases, `Cortex.Graph+`, `Cortex.Blueprint+`, `Cortex.UMG+`, rendered `Cortex.Editor+`, and full `Cortex+` sequentially; classify raw warnings and expected errors.
- [ ] Run staged offline MCP tests with uv --no-sync when the long-lived server locks the entrypoint. Never kill that server to reinstall.
- [ ] Audit toolkit guidance against orphan/enum/object-family/identity/animation/mouse changes. Update only stale contract guidance on a task branch and prove relevant shell checks; preserve unrelated toolkit branch.
- [ ] Update design/implementation records and verification report with exact revisions, results and limitations. Existing README changes retained; no unrelated docs sweep.
- [ ] Package exact main-to-candidate and correction diffs for independent review; resolve source-confirmed blockers with covering tests and scoped re-review.
- [ ] Recheck remote head/base, additively publish the reviewed branch to the original PR head, merge under normal repository policy, confirm merged SHA/state and reconcile scope in one evidence-backed comment.
- [ ] Safely fast-forward owning default branches if possible. Retain task branch if deletion needs force authority; leave parent gitlinks/user dirt uncommitted.
