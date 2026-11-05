# Issue 119 — accurate migration analysis and graph reads on UE 5.8

## Disposition and identity

- Request: https://github.com/etelyatn/CortexSandbox/issues/119 (reporter etelyatn, Ripper Blueprint→C++ migration audit 2026-11-04).
- Route: implement; merged UnrealCortex PR; parent `docs/systems` PR in CortexSandbox; toolkit guidance synced by cortex-toolkit #74. Parent gitlinks and pre-existing edits (`MCP/pyproject.toml`, `MCP/uv.lock`, `UnrealCortex.uplugin`) excluded.
- Plugin base `1ac0646fed0448103fa0e56f003fb61ff80e02a1`; branch `fix/issue119-migration-analysis`; candidate head `0638aaa1`.
- Design / plan: `docs/plans/2026-11-04-issue119-migration-analysis-{design,impl}.md` (D1–D7, final semantics recorded in the design).

## What shipped

- Widget trees are enumerated through `UWidgetTree::GetAllWidgets` (shared `CortexBPWidgetUtils::CollectWidgets`), so cold-loaded Widget Blueprints report their full tree; `widget_dependencies` lists placed Blueprint user widgets, ListView entry classes and widget-class variables, and `referenced_by_widgets` lists Widget Blueprint referencers from the Asset Registry.
- `analyze_for_migration`: no `"None"` events, deduplicated outputs, event-style interface implementations, CDO-backed empty defaults, debug-key bindings, any `UK2Node` supported, live-only `has_tick`, shared type/parent vocabulary, `sections` selector.
- `blueprint.list`: `type` / `exclude_types`, immediate + native parent, `asset_class`; one classifier for list/get_info/analyze/create.
- New read-only `blueprint.graph_metrics` (reachability-aware per-entry functional counts, conditionals, getter fan-out, unreachable nodes, ghost entries, budget violations).
- `graph.search_nodes`: `graph_name` scoping, optional filters (paged inventory), subclass / prefix `node_class`.
- `data.get_struct_schema`: object paths under any mounted root; non-struct assets refused before loading.
- MCP: complete-or-refuse for `analyze_for_migration` and `graph_metrics`; fixture + generated fallback synchronized.

## Acceptance matrix

| Criterion | Result | Evidence |
|---|---|---|
| Test that loads a saved WBP gets full widget tree and widget dependencies | PASS | `Cortex.Blueprint.Analysis.WidgetEntitiesAfterReload` (cold-load asserted: package unloaded before analyze; 7 widgets incl. non-variable panels and named-slot child), `Cortex.Blueprint.Analysis.WidgetDependencies` |
| `blueprint.list type:"Widget"` returns only WBPs; rows include immediate and native parent | PASS | `Cortex.Blueprint.List.FilterType/ExcludeTypes/ParentClassBlueprint`; live sandbox (103 rows, all Widget); live Ripper (316 Widget rows; 21 show BP parent ≠ native parent) |
| `graph_metrics` on BP with unconnected default Tick → `has_tick:false`, 0 logic for that entry | PASS | `Cortex.Blueprint.GraphMetrics.GhostTick`; live `BP_SimpleActor`: `has_tick:false`, ReceiveTick ghost with `functional_nodes:0` |
| Live check on Ripper `WBP_PatientMeetPopup`, `WBP_Patient_Body`, `BP_PlayerController` | PASS at `a7df63c2`; dependency delta native-only | RipperMirror editor on candidate `a7df63c2`: popup 21 widgets (issue's `get_tree` count), BPI functions 2; Body 42 events, none `"None"`; PlayerController 12 `debug_key` bindings, `graph_metrics` 28 live entries. Ripper list 829 assets (Object 349, Widget 316, Actor 110, AnimBlueprint 21, Interface 17, EditorUtility 10, Component 4, FunctionLibrary 1, Unknown 1 = CameraShake whose GameplayCameras parent module was not loaded). `references_widgets`/`referenced_by_widgets` (commit `0638aaa1`) not re-run on Ripper: the RipperMirror game module no longer compiled after concurrent upstream commits; proven by the native WidgetDependencies test |
| Analysis output defects (None events, dup outputs, BPI events, bool defaults, debug keys) | PASS | `Cortex.Blueprint.Analysis.{EventsExcludeNone,FunctionOutputsDeduplicated,InterfaceEventImplementations,BoolDefaultFromCDO,NonBoolDefaultsFromCDO,InputDebugKeyBinding}`; Ripper live above |
| `GameplayTagsK2Node_SwitchGameplayTag` not unsupported | PASS | `Cortex.Blueprint.Analysis.K2NodeSubclassSupported`; Ripper popup/body/PC `unsupported_node_types: []` |
| Compact paged node inventory; search_nodes graph_name/optional filter/subclass | PASS | `Cortex.Graph.SearchNodes.{GraphNameScope,NoFilterInventory,SubclassMatch}`; live 13-node inventory paged by cursor |
- Controller: `Cortex.Blueprint+` 290/290 @`6ff5688d` and 291/291 @`0638aaa1` (`AutomationTest_2026-11-05_085330.log`; one environment warning: engine `LogHttp` 3 s timeout probing `www.google.com/generate_204`, not plugin code); `Cortex.Core+` 205/205; Python non-e2e 1111 passed; `sync_fallback --check` OK.
| Optional `sections` with complete-or-refuse | PASS | `Cortex.Blueprint.AnalyzeForMigration.{SectionsSelector,UnknownSectionRefused}`; pytest router tests; live sectioned reads |

## Runs

- Controller: `Cortex.Blueprint+` 290/290 @`6ff5688d` and final run @`0638aaa1` (see PR); `Cortex.Core+` 205/205; Python non-e2e 1111 passed; `sync_fallback --check` OK.
- Implementers: `Cortex.Graph+` 324/324, `Cortex.Data+` 134/134 @`e2c4bf85` (no later Graph/Data changes); `Cortex.Blueprint+` 291/291 @`0638aaa1`.
- Environment: intermittent full-suite editor crash (60 s Blueprint-compile stall, then `Background Worker crashed`) while another editor was running; confirmed by the maintainer as a known environment flake; affected runs were rerun clean.

## Limitations

- Positive load of a non-`/Game` user-defined struct is untested (no such asset in the sandbox); refusal paths are tested.
- `graph_metrics` measures K2-derived graphs only (animation and state-machine graphs skipped); macro graphs are counted at instance sites.
