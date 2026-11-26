# Issue 266 — PIE world targeting verification

**Issue:** https://github.com/etelyatn/UnrealCortex/issues/266
**Candidate:** `TBD` on `feat/266-pie-world-targeting` (feature `9820618c`, verification/fixes follow)
**Base:** `2df9a88c98ca6d31d408a7aacf9752f3ef8ca240` (`main` after PR #270; merged into the branch as `d59839f9`)
**Version:** plugin 0.4.27 (MCP 0.2.21)
**Date:** 2026-11-26
**Environment:** Windows 11, UE 5.8.3 (`5.8.3-58210709+++UE5+Release-5.8`), MSVC 14.44

## Scope

Add `world: "editor" | "pie"` to the actor-scoped CortexLevel commands, PIE-safe delete and
transaction behavior, and `level.set_pie_view` for runtime screenshots. See the design and
implementation docs in `docs/plans/2026-11-26-issue266-pie-world-targeting-*.md`.

## Native automation evidence

| Stage | Command | Result | Log |
|---|---|---|---|
| RED | `RunTests.ps1 'Cortex.Level.PIE+'` | 0/3 passed — `spawn_actor` refused `world` as `INVALID_FIELD`; `set_pie_view` was `UNKNOWN_COMMAND`; validation expected `PIE_NOT_ACTIVE`/`INVALID_VALUE` | `Saved/task/issue266-logs/01-RED-world-param.log` |
| GREEN | `RunTests.ps1 'Cortex.Level.PIE+'` | 3/3 passed | `Saved/task/issue266-logs/02-GREEN-pie.log` |
| Regression | `RunTests.ps1 'Cortex.Level+'` | 82/82 passed | `Saved/task/issue266-logs/03-GREEN-full-level.log` |
| GREEN after final validation change | `RunTests.ps1 'Cortex.Level.PIE+'` | 3/3 passed | `Saved/task/issue266-logs/04-GREEN-pie-final.log` |
| Merged base regression | `RunTests.ps1 'Cortex.Level+'` after merging plugin main (`d59839f9`) | 82/82 passed | `Saved/task/issue266-logs/05-GREEN-merged-full-level.log` |
| RED for batch transaction isolation | Same PIE tests with `FCortexLevelPIETransactionScope` temporarily disabled | 2/3 passed — the editor-world batch write was not journaled and the batch committed no undo entry | `Saved/task/issue266-logs/06-RED-batch-transaction.log` |
| GREEN review fixes | `RunTests.ps1 'Cortex.Level.PIE+'` | 3/3 passed | `Saved/task/issue266-logs/07-GREEN-review-fixes.log` |
| GREEN review fixes regression | `RunTests.ps1 'Cortex.Level+'` | 82/82 passed | `Saved/task/issue266-logs/08-GREEN-review-fixes-full-level.log` |

The PIE tests route through `FCortexCommandRouter`, so the declared `world` parameter is
validated by the same contract the live MCP route uses. They exercise:

- `PIE_NOT_ACTIVE` for `world: "pie"`, `PIE`, and `set_pie_view` without a session;
  `INVALID_VALUE` for `"game"`, a non-string, an empty string, an out-of-range `set_pie_view`
  FOV, and a malformed top-level batch `world` (the editor-world target survives).
- Spawn into `GEditor->PlayWorld`, found in the PIE world and absent from the editor world;
  `set_transform`, `set_actor_property`/`get_actor_property`; a batched `set_tags` with a
  top-level `world` (preflight + commit); `find_actors`/`get_info` on the PIE world;
  `duplicate_actor` refusal in single and batched form (batch fails at `preflight_failed`);
  `delete_actor` removal; default editor-world spawn/delete while PIE runs.
- A `core.batch` containing an editor-world `set_transform` and a PIE `set_transform`:
  the editor write is journaled and the batch commits an undo entry; the PIE actor is not in
  the editor transaction buffer. The RED run (scope disabled) showed the engine classifies
  the whole transaction as transient because it contains a PIE record and removes it, losing
  the editor-world undo entry.
- `set_pie_view` first call spawns a tagged camera, sets the view target and FOV; second call
  reuses the same camera, moves it, and updates FOV.

### Build and static checks

| Check | Result |
|---|---|
| `UnrealBuildTool CortexSandboxEditor Win64 Development` (merged base + review fixes) | Succeeded |
| `python cli/clang/clang-syntax-check.py` (changed files vs `origin/main`, after review fixes) | 25 passed, 0 failed, 0 errors, 0 warnings |

## Live MCP evidence

Task-owned editor on the final merged/fixed binaries: PID 38160, port 8742, project
`CortexSandbox`, plugin 0.4.27.

| Step | Observation |
|---|---|
| `level.get_info` with `world: "pie"` before PIE | `PIE_NOT_ACTIVE` with the shared recovery message |
| `level.delete_actor` with `{"world":123,"items":[...]}` | `INVALID_VALUE` "world must be 'editor' or 'pie', got '123'" before any item |
| `editor.start_pie` (`selected_viewport`) | `state: playing` |
| `level.spawn_actor` (`world: "pie"`, `TargetPoint`, label `CortexLivePIE2_170917`) | `TargetPoint_0` at `[1200,2400,400]`; editor `find_actors` empty; PIE `find_actors` count 1 |
| `level.set_transform` (`world: "pie"`) | `get_actor` reports `[1300,2400,450]`, yaw 135 |
| `level.set_pie_view` first call | `CameraActor_0`, `reused: false`, `view_target_set: true`, FOV 75 |
| `level.set_pie_view` second call | same `CameraActor_0`, `reused: true`, moved to `[0,0,5000]`, FOV 90 |
| `editor.capture_screenshot` | `Saved/task/issue266b-pie-view.png`, 1467×1016, 1,189,016 bytes, `view: pie_game_camera` |
| Alternative view screenshot | `Saved/task/issue266b-pie-view-alt.png`, 1467×1016, 1,235,711 bytes; different MD5 (`189a337d…` vs `28994aeb…`), proving the view target changed the capture |
| `level.duplicate_actor` (`world: "pie"`) | `INVALID_OPERATION` with the `spawn_actor` guidance |
| `level.delete_actor` (`world: "pie"`) | actor removed; PIE `find_actors` empty |
| `editor.stop_pie` | `state: stopped`; editor `find_actors` still empty (no editor-world contamination) |

Earlier pre-merge live evidence (plugin 0.4.25, PID 53828, port 8743) is retained under
`Saved/task/issue266-*.json` and `issue266-pie-view*.png`; the final run above is
`issue266b-*`.

## Review corrections

An independent review found two issues that were fixed before merge:

1. **`core.batch` transaction isolation.** The router opens one transaction for the whole
   batch. PIE world clones retain `RF_Transactional`, so without detaching `GUndo` the
   transaction contained a PIE record; the engine's `FTransaction::IsTransient()` then
   classified the entire batch as transient and removed it at finalize, losing the batch's
   editor-world undo entry. `FCortexLevelPIETransactionScope` now detaches `GUndo` for every
   PIE write operation, including engine setters such as `SetActorLabel`/`SetFolderPath`.
   The regression test proves the editor write is journaled and an undo entry is committed;
   the RED run with the scope disabled failed both assertions.
2. **Malformed batched `world`.** A non-string/empty/unknown top-level `world` in a batch
   was silently ignored and the items fell back to the editor world. `ApplyWorldToBatchRequest`
   now returns the same `INVALID_VALUE` refusal as the single-target shape before any item
   runs; the test proves a target actor survives.
3. The duplicate refusal now also fails at batch preflight (instead of during commit), so a
   PIE duplicate cannot partially commit a batch.

## Limits

- The native automation runner in this workspace uses `-NullRHI` for `Cortex.Level+`; the PIE
  view-target/FOV assertions passed there, and the live rendered editor produced the screenshots.
- `capture_screenshot` reported `camera_provenance: unavailable` while `view: pie_game_camera`;
  that provenance field is existing capture behavior and is not part of this change. The captured
  pixels differ between two camera poses, so the view change is observable.
- Multi-client PIE is not covered: `world: "pie"` targets `GEditor->PlayWorld` and its first
  player controller.
- `level_compose` does not accept `world`; multi-actor PIE work uses individual `level_cmd`
  calls. The router's native `batch` is not exposed as a `core_cmd` command.
- The parent CortexSandbox workspace system docs (`docs/systems/cortex-level.md`,
  `docs/systems/INDEX.md`) are updated separately in that repository; this plugin report
  covers the plugin implementation.

## Remaining scope

None for issue #266. Editor-only commands (selection, grouping, data layers, streaming,
saving, lifecycle) intentionally keep the editor world and do not declare `world`.
