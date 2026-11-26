# Issue 266 — PIE world targeting for level actor commands

**Status:** Approved for implementation (maintainer request in issue #266)
**Owner:** UnrealCortex / CortexLevel
**Date:** 2026-11-26
**Issue:** https://github.com/etelyatn/UnrealCortex/issues/266

## Problem

`level.*` actor commands always resolve the world through
`FCortexLevelUtils::GetEditorWorld`, which returns the `EWorldType::Editor` context.
While PIE is running, there is no way to spawn, find, move, or configure an actor in
the play world. Python cannot replace this: `unreal.GameplayStatics` exposes no
deferred spawn, and `EditorActorSubsystem.spawn_actor_from_class` targets the editor
world.

The concrete maintenance use case is a runtime screenshot: place a camera next to a
gameplay object in PIE, make it the view target, capture. Today the only workaround is
to find an unrelated camera already in the play world.

## Contract

Add an optional `world` parameter to the level commands that operate on an actor or
the current world:

- `"editor"` (default) — existing behaviour, `FCortexLevelUtils::GetEditorWorld`.
- `"pie"` — `GEditor->PlayWorld` through the shared `FCortexPIEUtils::GetPIEWorld`
  helper. Refused with `PIE_NOT_ACTIVE` when PIE is not running.

Any other value is refused with `INVALID_VALUE` before any lookup or write.

### Commands that adopt `world`

| Group | Commands |
|---|---|
| Actor lifecycle | `spawn_actor`, `delete_actor`, `rename_actor` |
| Actor read/write | `get_actor`, `set_transform`, `set_actor_property`, `get_actor_property` |
| Components | `list_components`, `add_component`, `remove_component`, `get_component_property`, `set_component_property` |
| Organization | `attach_actor`, `detach_actor`, `set_tags`, `set_folder` |
| Queries | `list_actors`, `find_actors`, `get_bounds` |
| World info | `get_info` |

`duplicate_actor` declares `world` so the refusal is explicit, but only `editor`
is supported: the editor duplicate path depends on editor-world selection and copy/
paste state. `world: "pie"` returns `INVALID_OPERATION` with a pointer to
`spawn_actor`.

Commands that are inherently editor-only keep targeting the editor world and do not
declare `world`: selection (`select_actors`, `get_selection`), actor grouping,
data layers, streaming sublevels, level saving, and level lifecycle.

Batched write commands (`delete_actor`, `rename_actor`, `attach_actor`,
`detach_actor`, `set_tags`, `set_folder`, `set_component_property`) apply a
top-level `world` to every item whose own params do not set one. A per-item
`world` overrides the top-level value. Preflight and commit therefore resolve the
same world for each item.

### PIE write semantics

- `delete_actor` uses `UWorld::DestroyActor(Actor, false, false)` in PIE.
  `UEditorActorSubsystem::DestroyActor` refuses non-editor worlds
  (`InternalActorUtilitiesSubsystemLibrary::IsEditorLevelActor`).
- Write commands open an `FScopedTransaction` only for the editor world. PIE writes
  are transient and must not put PIE objects in the editor transaction buffer,
  because PIE teardown then resets that buffer and discards editor undo history.
- `spawn_actor` sets `RF_Transactional` only for editor-world spawns.
- Actor labels and folders are editor metadata but remain legal on PIE actors; they
  are useful for identifying transient test actors.

### `set_pie_view`

New `level.set_pie_view` command for the screenshot path:

```
level_cmd(command="set_pie_view", params={
  "location": [x, y, z],
  "rotation": [pitch, yaw, roll],
  "fov": 75            # optional, default 90
})
```

- Requires a running PIE session; otherwise `PIE_NOT_ACTIVE`.
- Requires a PIE world with a first local player controller; otherwise
  `INVALID_OPERATION`.
- Spawns a transient `ACameraActor` tagged `CortexPIEViewCamera` on first use and
  reuses it on later calls.
- Applies location/rotation and sets the camera component FOV.
- Calls `APlayerController::SetViewTargetWithBlend(Camera, 0)` and updates the
  player camera manager immediately so a screenshot in the same MCP turn sees the
  new pose.
- Returns `camera`, `label`, `reused`, `location`, `rotation`, `fov`, and
  `view_target_set`.

## Non-goals

- PIE duplicate semantics (runtime duplicate is not equivalent to editor copy/paste).
- Multi-client PIE selection (the command targets `GEditor->PlayWorld` and its first
  player controller).
- `level_compose` batch PIE support; callers can use `core.batch` with `world: "pie"`
  on each `level.*` step.
- Changing `editor.capture_screenshot`; it already reads the active PIE view.

## Risks

- A stale `PlayWorld` during teardown would accept a write; `PlayWorld` is nulled
  synchronously by the engine before command dispatch resumes, so the helper relies on
  the same ground truth as `CortexEditorPIEState`.
- `set_pie_view` mutates runtime camera state outside the game's own camera manager;
  this is intentional for screenshots and is documented as a debug helper.
- Adding `CortexEditor` as a private dependency of `CortexLevel` couples a domain to
  shared editor infrastructure. The architecture already permits domain → shared
  infrastructure dependencies, and the alternative duplicates the PIE-world and
  `PIE_NOT_ACTIVE` helper.
