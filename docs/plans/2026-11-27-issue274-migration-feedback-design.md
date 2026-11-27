# Issue 274 design: Widget Blueprint to C++ migration feedback

**Request:** [UnrealCortex#274](https://github.com/etelyatn/UnrealCortex/issues/274). Twenty-one items and one
section on stale embedded widget templates, collected while migrating five Widget Blueprints to native parents
(plugin 0.4.20). Triage against `main` 0.4.29 (`70331f2a`) found every item still open in source; #8, #16, #2 and
#20 are partly covered. Delivery: every item, as themed slices.

## Decisions per slice

### A. Editor and core lifecycle (items 1, 3, 4, 5, 7, 8, 14, 20)

| Item | Contract |
|---|---|
| 1 `editor_restart` | The engine comes from the running editor: `psutil.Process(old_pid).exe()` (`<root>/Engine/Binaries/Win64/UnrealEditor.exe`). Fallback: the `.uproject` `EngineAssociation` (a directory, or a registry build id / installed version). Unresolvable → `INVALID_OPERATION`, nothing launched. The `UE_56_PATH` variable is removed. The old pid comes from the connection, else from the `Saved/CortexPort-*.txt` file for the connected port; unknown → refused before shutdown. Success requires: old pid exited, launched `Popen.pid` alive, a port file with `pid == Popen.pid != old_pid`, and `get_status.project_name` equal to the project. Result reports `old_pid`, `new_pid`, `engine_path`. New `force` argument (default `false`) is passed to `core.shutdown`; an `UNSAVED_CHANGES` refusal is returned unchanged and nothing is launched. |
| 3 `start_pie`/`restart_pie` `mode` | Enum `selected_viewport` (default) or `new_window`. `new_window` launches PIE in a floating window (no destination viewport, play mode `PlayMode_InEditorFloating`). Any other value → `INVALID_FIELD` naming both values, no PIE state change. Description lists the values. |
| 4 `core.shutdown` | `force` defaults to `false`. With dirty packages and no `force` → `UNSAVED_CHANGES`, `details.dirty_packages` (same entries as item 14) and `details.count`; no exit is armed. `force: true` keeps discarding and reports `discarded_packages`. |
| 14 `core.list_dirty_packages` | New read-only command, no params. `count`, `packages: [{package, file}]` (`file` empty for never-saved packages). Transient and script packages excluded. One helper shared with item 4. |
| 5 `execute_console_command` | Runs in the PIE world when PIE is active, else in the editor world; `EDITOR_NOT_READY` only when neither exists. Dispatch order: `World->Exec`, console manager, `GEngine->Exec` (handles `stat`). Response adds `world` (`pie`/`editor`) and `route` (`world_exec`, `console`, `engine_exec`). |
| 7 `capture_screenshot` | New `source`: `auto` (default; the PIE viewport while PIE runs, else the active level viewport), `editor`, `pie` (`PIE_NOT_ACTIVE` without PIE). The PIE capture reads `GEditor->GetPIEViewport()` (selected-viewport and new-window PIE alike) through `FSlateApplication::TakeScreenshot` of its viewport widget, so UMG is included. Response adds `source` and `includes_ui`. Editor-viewport capture is unchanged (scene only, `includes_ui: false`). |
| 8 `get_recent_logs` | New `contains`: case-insensitive substring match on the message, combinable with `category`/`severity`. Unknown params are already refused by the router (issue 255). |
| 20 `inject_mouse` | Documented limitation in the command description and docs: moves are Slate pointer events; code that reads the OS cursor through the scene viewport (`GetMousePosition`, deprojection) does not see them; use a Cortex Replay recording for cursor-driven gameplay. |

### B. Graph authoring (items 9, 11, 12, 13, 15, 16 node half, 17)

| Item | Contract |
|---|---|
| 9 | A `CallFunction` with `call_kind: interface_message` (patch and `add_node`) creates `UK2Node_Message`. The patch accepts the message family (`K2Node_Message`) with `function_name` (+ optional `owner_class`). A non-interface function with `interface_message` stays refused. |
| 11 | `graph.get_subgraph` gains `out_path`: the complete response is written with the safe-file contract; the reply is a summary (`out_path`, `node_count`, `bytes`, and `edge_count` with `include_edges=true`). The Python size refusal names `out_path`. |
| 12 | Serialized `K2Node_CreateDelegate` nodes report `function_name` and `scope_class`. |
| 13 | Serialized `K2Node_VariableGet` nodes report `variation`: `pure`, `branch`, `validated_object`. `graph_metrics` counts the `branch` and `validated_object` variations as conditionals. |
| 15 | `prune_island`: reroute knots whose every link stays inside the removed set are removed with the island (no `additional_node_guids`). New opt-in `include_result_producers: true` also removes pure nodes whose only consumers are removed nodes or a retained function result node; the severed result pins revert to their defaults and are journaled. |
| 16 | New `graph.remove_nodes`: `node_ids[]` or `keep_node_ids[]` (exactly one), `dry_run`, guards as `remove_node`. One transaction; entry/result/non-deletable nodes are never removed and are reported in `kept`. |
| 17 | Patches with `target.implementation` can address the reserved locator `result`; a new `disconnect` array (endpoint pairs as in `connect`) runs before `connect`, so the default entry→result exec link can be replaced in one apply. |

### C. Blueprint batch commands (items 13 metrics owner in B, 16 graph half, 18)

| Item | Contract |
|---|---|
| 16 | `blueprint.remove_graph` accepts `names[]` instead of `name`. The preview lists every graph and returns one fingerprint and one validation hash over the set; apply removes all or none (one guarded run). |
| 18 | `adopt_native_member` plans `engine_already_adopted` for a same-named native property that is not Blueprint-visible when the Blueprint variable no longer exists. The refusal remains when the variable still exists. |

### D. QA, Reflect, UMG rename (items 6, 10, 19)

| Item | Contract |
|---|---|
| 6 | `qa.look_at` aims from the camera (`PlayerCameraManager::GetCameraLocation`, else `APawn::GetPawnViewLocation`). Response adds `view_origin`. |
| 10 | Every `out_path` summary that caps an array sets `truncated: true` and `truncated_arrays: [...]`; uncapped summaries report `truncated: false`. |
| 19 | `umg.rename_widget` reports `display_label_before`, `display_label_after`, `display_label_changed` (the engine rename sets the label). |

### E. Reparent safety and embedded widget templates (item 2, 21, embedded section)

| Item | Contract |
|---|---|
| 2 | The reparent guarded run journals the target CDO and restores it after a failed compile. The failure response lists `reload_packages`: other loaded packages that the failed compile dirtied, with the instruction to reload them from disk. |
| 21 | The reparent plan reports `type_changing_call_sites`: graph nodes whose target member resolves to a binding or variable whose type changes to a base type under the new parent, with count, graph and node ids. Non-blocking warning. |
| E-a | New read-only `umg.get_embedded_widget_diff`: for an outer Widget Blueprint (`asset_path`), every embedded `UUserWidget` instance with the properties whose template value differs from its class default, including defaults-only native properties (`property`, `declared_in`, `instance_value`, `class_default_value`, `instance_editable`). |
| E-b | `umg.get_embedded_widget_values` includes defaults-only (`DisableEditOnInstance`) properties declared below `UUserWidget`, each with `instance_editable`. |
| E-c | `blueprint.set_class_defaults` and `blueprint.reparent` on a Widget Blueprint report `embedded_instances`: loaded outer Widget Blueprints whose templates of that class still hold the old class default for a changed property (`asset_path`, `widget_name`, `properties`). Opt-in `resync_embedded: true` copies the new default into exactly those instances (never into intentional overrides — the engine details-panel rule) and marks the outer Blueprints modified; their save follows the command's `save`. |
| E-d | Before a reparent applies, the overrides of loaded embedded instances are inventoried; after apply, `dropped_overrides` lists instance properties that no longer exist on the class. |

CortexBlueprint already depends on the engine `UMG` module; embedded scanning uses engine `UWidgetBlueprint`/`UWidgetTree` APIs. CortexBlueprint does not depend on CortexUMG; the small template-scan helper is implemented in each module rather than coupling domains.

## Shared contracts and generated artifacts

- Every new param/command is declared in `GetSupportedCommands`; the router rejects undeclared params.
- `MCP/tests/fixtures/capabilities_cache_full.json` and `_fallback_generated.py` are regenerated once at integration.
- System docs (`CortexSandbox/docs/systems/`) are updated at integration.
