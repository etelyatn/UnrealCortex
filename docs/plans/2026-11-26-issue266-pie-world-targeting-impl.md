# Issue 266 — PIE world targeting implementation

**Design:** `2026-11-26-issue266-pie-world-targeting-design.md`
**Issue:** https://github.com/etelyatn/UnrealCortex/issues/266
**Owner:** UnrealCortex / CortexLevel

## Changes

### 1. `FCortexLevelUtils` (`Source/CortexLevel/Private/CortexLevelUtils.*`)

- Add `ResolveWorld(Params, OutError)`:
  - absent/`editor` → `GetEditorWorld`;
  - `pie` → `FCortexPIEUtils::GetPIEWorld()`, `PIENotActive` when null;
  - unknown → `INVALID_VALUE` naming the accepted values.
- Add `ShouldTransact(World)` → true unless `WorldType == EWorldType::PIE`.
- Add `ApplyWorldToBatchRequest(Params, Request)`: copy a top-level `world` into
  every batch item that does not already set one.
- `CortexLevel.Build.cs` adds the `CortexEditor` private dependency for
  `FCortexPIEUtils`.

### 2. World-aware operations

Replace `GetEditorWorld(Error)` with `ResolveWorld(Params, Error)` in:

- `CortexLevelActorOps.cpp`: spawn, delete, rename; batch preflight
  (`ResolveWorld(Item.Params, Error)`); batch world propagation.
- `CortexLevelTransformOps.cpp`: get actor, set transform, set/get actor property.
- `CortexLevelComponentOps.cpp`: list/add/remove component, get/set component
  property; batch preflight and propagation.
- `CortexLevelOrganizationOps.cpp`: attach, detach, set tags, set folder; batch
  preflight and propagation.
- `CortexLevelQueryOps.cpp`: list actors, find actors, get bounds.
- `CortexLevelStreamingOps.cpp`: get info.

Gate every `FScopedTransaction` in these paths with
`FCortexLevelUtils::ShouldTransact(World)`.

PIE delete path in `DeleteActor`: `World->DestroyActor(Actor, false, false)`.

PIE duplicate refusal in `DuplicateActor`:
`INVALID_OPERATION`, message points to `spawn_actor`.

`GetInfo` warns that PIE is active only when the editor world was requested.

### 3. `set_pie_view`

- Declared and routed in `CortexLevelCommandHandler.cpp`.
- Implemented in `CortexLevelActorOps.cpp`.
- Camera tag `CortexPIEViewCamera`.
- Response: `camera`, `label`, `reused`, `location`, `rotation`, `fov`,
  `view_target_set`.

### 4. Command declarations

Add `.Optional("world", "string", ...)` to the 20 commands listed in the design.
`duplicate_actor` declares it so the `world: "pie"` refusal is attributable.
Add the `set_pie_view` declaration with required `location`/`rotation` and optional
`fov`.

### 5. Tests

`Source/CortexLevel/Private/Tests/CortexLevelPIECommandsTest.cpp`:

- `Cortex.Level.PIE.WorldValidation` — no-PIE refusal `PIE_NOT_ACTIVE` (including
  case-insensitive `PIE`), unknown value `INVALID_VALUE`, and no editor-world spawn
  from the refused request.
- `Cortex.Level.PIE.ActorCommands` — latent PIE session:
  - spawn into PIE, find it in `GEditor->PlayWorld` and not in the editor world;
  - `set_transform`, `set_actor_property`/`get_actor_property`;
  - batch `set_tags` with top-level `world: "pie"` (preflight + commit);
  - `find_actors` and `get_info` see the PIE world;
  - `duplicate_actor` refuses PIE;
  - `delete_actor` removes it from PIE;
  - without `world`, spawn/delete still target the editor world while PIE runs.
- `Cortex.Level.PIE.SetView` — latent PIE session:
  - first call spawns the tagged camera and sets the view target/FOV;
  - second call reuses the same camera and updates pose/FOV.

Tests route through `FCortexCommandRouter` so the declared `world` parameter is
validated, not only the handler.

### 6. Documentation and toolkit

- `docs/systems/cortex-level.md`: PIE targeting section, dependency, tool table.
- `docs/systems/INDEX.md`: CortexLevel dependency line and domain description.
- Plugin verification report.
- `cortex-toolkit`: `skills/cortex-level/SKILL.md`,
  `resources/level-patterns.md`, `resources/level-design.md`,
  `resources/mcp-tool-reference.md`, regenerated MCP surface snapshot.

## Verification

1. RED: build with the new tests; `Cortex.Level.PIE+` fails on the undeclared
   `world` parameter / unknown command.
2. GREEN: `Cortex.Level.PIE+` passes.
3. Regression: full `Cortex.Level+` suite.
4. Clang syntax gate on changed C++ files.
5. Live MCP smoke in a task-owned editor:
   `editor.start_pie` → `level.spawn_actor(world="pie")` →
   `level.set_transform(world="pie")` → `level.set_pie_view` →
   `editor.capture_screenshot` → `level.delete_actor(world="pie")` →
   `editor.stop_pie`; validate the screenshot file and the returned actor/camera
   state.

## Rollback

The new parameter is optional and defaults to the previous editor-world path.
`set_pie_view` is additive. Reverting the commit restores the old surface without
data migration.
