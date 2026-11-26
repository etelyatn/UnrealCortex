# Issue 266 — PIE world targeting verification

**Issue:** https://github.com/etelyatn/UnrealCortex/issues/266
**Candidate:** `9820618cf9f402286a834d9853b9b8c7d1e10c2b` on `feat/266-pie-world-targeting`
**Base:** `0fe4531bc7967684c06b48efef17d03ba50c8584` (`main`, plugin v0.4.25)
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

The PIE tests route through `FCortexCommandRouter`, so the declared `world` parameter is
validated by the same contract the live MCP route uses. They exercise:

- `PIE_NOT_ACTIVE` for `world: "pie"` and `PIE` without a session; `INVALID_VALUE` for
  `"game"`, a non-string, and an empty string; no editor-world actor from a refused spawn.
- Spawn into `GEditor->PlayWorld`, found in the PIE world and absent from the editor world;
  `set_transform`, `set_actor_property`/`get_actor_property`; a batched `set_tags` with a
  top-level `world` (preflight + commit); `find_actors`/`get_info` on the PIE world;
  `duplicate_actor` refusal; `delete_actor` removal; default editor-world spawn/delete while
  PIE runs.
- `set_pie_view` first call spawns a tagged camera, sets the view target and FOV; second call
  reuses the same camera, moves it, and updates FOV.

### Build and static checks

| Check | Result |
|---|---|
| `UnrealBuildTool CortexSandboxEditor Win64 Development` | Succeeded |
| `python cli/clang/clang-syntax-check.py` (changed files vs `origin/main`) | 25 passed, 0 failed, 0 errors, 0 warnings |

## Live MCP evidence

Task-owned editor: PID 53828, port 8743, project `CortexSandbox`, plugin 0.4.25.

| Step | Observation |
|---|---|
| `level.get_info` with `world: "pie"` before PIE | `PIE_NOT_ACTIVE` with the shared recovery message |
| `editor.start_pie` (`selected_viewport`) | `state: playing` |
| `level.spawn_actor` (`world: "pie"`, `TargetPoint`, label `CortexLivePIE_155017`) | `TargetPoint_0` at `[1200,2400,400]`; editor `find_actors` empty; PIE `find_actors` count 1 |
| `level.set_transform` (`world: "pie"`) | `get_actor` reports `[1300,2400,450]`, yaw 135 |
| `level.get_info` without `world` | `world_type: Editor`, warning names the active PIE session |
| `level.get_info` with `world: "pie"` | `world_type: PIE`, `UEDPIE_0_TestMap`, 26 actors |
| `level.set_pie_view` first call | `CameraActor_0`, `reused: false`, `view_target_set: true`, FOV 75 |
| `level.set_pie_view` second call | same `CameraActor_0`, `reused: true`, moved, FOV 70 |
| `editor.capture_screenshot` | `Saved/task/issue266-pie-view.png`, 1467×1016, 1,185,583 bytes, `view: pie_game_camera` |
| Alternative view screenshot | `Saved/task/issue266-pie-view-alt.png`, 1467×1016, 1,168,748 bytes; different MD5 (`d9c6047b…` vs `2930d201…`), proving the view target changed the capture |
| `level.duplicate_actor` (`world: "pie"`) | `INVALID_OPERATION` with the `spawn_actor` guidance |
| `level.delete_actor` (`world: "pie"`) | actor removed; PIE `find_actors` empty |
| `editor.stop_pie` | `state: stopped`; editor `find_actors` still empty (no editor-world contamination) |

Evidence files: `Saved/task/issue266-spawn.json`, `issue266-view1.json`, `issue266-view2.json`,
`issue266-screenshot.json`, `issue266-pie-view.png`, `issue266-pie-view-alt.png`.

## Limits

- The native automation runner in this workspace uses `-NullRHI` for `Cortex.Level+`; the PIE
  view-target/FOV assertions passed there, and the live rendered editor produced the screenshots.
- `capture_screenshot` reported `camera_provenance: unavailable` while `view: pie_game_camera`;
  that provenance field is existing capture behavior and is not part of this change. The captured
  pixels differ between two camera poses, so the view change is observable.
- Multi-client PIE is not covered: `world: "pie"` targets `GEditor->PlayWorld` and its first
  player controller.
- `level_compose` does not accept `world`; multi-actor PIE work uses `core.batch` with per-step
  `world: "pie"` or individual `level_cmd` calls.

## Remaining scope

None for issue #266. Editor-only commands (selection, grouping, data layers, streaming,
saving, lifecycle) intentionally keep the editor world and do not declare `world`.
