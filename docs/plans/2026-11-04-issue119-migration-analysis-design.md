# Accurate migration analysis and graph reads on UE 5.8

Date: 2026-11-04
Status: Delivery authorized by user on 2026-11-04 (merged delivery; scope #119 → toolkit #74 → #120)
Issue: https://github.com/etelyatn/CortexSandbox/issues/119
Reporter: etelyatn (Ripper Blueprint→C++ migration audit, 2026-11-04)
Implementation owner: UnrealCortex / CortexBlueprint, CortexGraph, CortexData (+ MCP Python facade)
Delivery endpoint: verified, reviewed, merged UnrealCortex PR; toolkit reference sync travels with cortex-toolkit #74; parent CortexSandbox gitlinks excluded.

## Intent and acceptance

Migration plans built from Cortex reads must reflect what the Blueprint actually contains after a cold load on UE 5.8, and logic metrics must count only logic that can run.

| Criterion | Current evidence (source) | Route | Minimal proof and exit |
|---|---|---|---|
| Loaded WBP returns full widget tree and widget dependencies from analyze_for_migration | `ExtractWidgetEntities` reads `UWidgetTree::AllWidgets` by reflection (CortexBPAnalysisOps.cpp:681); UE 5.8 `UWidgetTree::PostLoad` empties it (WidgetTree.cpp:423). Same in CortexBPToolbarExtension.cpp:156 | Implement | Native test: create WBP with rooted panel+children, SavePackage, purge, reload, analyze → widgets and widget_dependencies populated |
| events contain no `"None"` | `EventReference.GetMemberName().ToString()` of custom/bound events (AnalysisOps:1811) | Implement | Native test with custom event + component-bound event |
| Function outputs listed once | every `UK2Node_FunctionResult` appends its pins (AnalysisOps:1621) | Implement | Native test: function with two return nodes |
| Event-style BPI implementations listed | only `InterfaceDesc.Graphs` read (AnalysisOps:2120) | Implement | Native test: BPI function without outputs implemented as event |
| Bool default_value not `""` | raw `FBPVariableDescription::DefaultValue` (AnalysisOps:1486, :1617) | Implement | Native test: untouched bool member reports `"false"` |
| Debug-key events in input_bindings | `DetectInputBindings` handles Enhanced Input nodes only, early-returns | Implement | Native test with `K2Node_InputDebugKey` |
| `blueprint.list type` honoured; immediate + native parent per row; `exclude_types` | `List` reads only `path` (CortexBPAssetOps.cpp:1044); parent prefers NativeParentClassPath (:1096) | Implement | Native tests: type filter, exclude, BP→BP parent |
| AnimBP / EUB / non-Actor UObject Blueprints not `Unknown` | classifier vocabulary limited (AssetOps:383-418, 477-552) | Implement | Native test per class family available in the engine |
| `graph_metrics`: unconnected default Tick → `has_tick:false`, 0 logic for that entry | `has_tick` true for any Tick event node (AnalysisOps:1816, :1308) | Implement (new command) | Native test; also analyze_for_migration has_tick uses liveness |
| `GameplayTagsK2Node_SwitchGameplayTag` not "unsupported" | prefix allow-list `K2Node_` (AnalysisOps:1250, :1322) | Implement | Native test with a UK2Node subclass whose class name lacks `K2Node_` prefix |
| Compact paged node inventory; search_nodes honours graph_name, no mandatory filter, subclass/prefix class match | CortexGraphNodeOps.cpp:799-962 | Implement | Native tests + Python cursor paging of unfiltered search over >40 kB |
| `data.get_struct_schema` accepts full object paths | short-name iterator only (CortexDataTableOps.cpp:775-804) | Implement | Native test with `/Script/...` and `/Game/...` user struct path |
| Optional `sections` selector with complete-or-refuse | only asset_path parsed | Implement (cheap; reused by #120 export) | Native test: selected sections only; unknown section refused |
| Live check on Ripper `WBP_PatientMeetPopup`, `WBP_Patient_Body`, `BP_PlayerController` | reported | Runtime acceptance | Run against a Ripper editor built with the candidate (RipperMirror checkout); record counts |

Excluded: classic (non-Enhanced) input nodes in input_bindings (not requested). No item is already fixed on base 1ac0646f.

## Current source basis

Plugin base: 1ac0646fed0448103fa0e56f003fb61ff80e02a1 (equals remote main). Branch `fix/issue119-migration-analysis`. Pre-existing uncommitted release edits (`MCP/pyproject.toml`, `MCP/uv.lock`, `UnrealCortex.uplugin`) are user work and are never staged.

## Decisions

### D1 Widget enumeration
CortexBlueprint gains private dependency `UMG` (CortexGraph already depends on UMG; no domain coupling). One private helper `CortexBPWidgetUtils::CollectWidgets(const UBlueprint*) -> TArray<UWidget*>` reads the `WidgetTree` object property by reflection (WBP class stays in UMGEditor, not linked), casts to `UWidgetTree` and calls `ForEachWidget` (includes named-slot content). Output order: tree pre-order (deterministic). Both `ExtractWidgetEntities` and the toolbar extension use it. No reader of `AllWidgets` remains.

### D2 analyze_for_migration output fixes
- `events`: never emit a `None` member name. `UK2Node_ComponentBoundEvent` emits `"<ComponentPropertyName>.<DelegatePropertyName>"`. Custom events stay only in `custom_events`.
- Function `outputs`: union of result-node user pins, deduplicated by pin name, first-seen order.
- `interfaces_implemented[].functions`: function-graph implementations plus ubergraph `UK2Node_Event` nodes whose `EventReference` parent class is the interface (or child). Entries stay strings (member name), deduplicated.
- `default_value`: member variable with empty description default → `ExportText` of the property on the generated class CDO (bool → `"false"`/`"true"`). Local variable with empty default and bool type → `"false"`; other empty locals unchanged (type default, as the editor shows).
- `input_bindings`: add `UK2Node_InputDebugKey` (lazy class lookup `/Script/InputBlueprintNodes.K2Node_InputDebugKey`, reflection reads) emitting `{ "kind":"debug_key", "key":<FKey name>, "modifiers":{ "ctrl","shift","alt","cmd" }, "execute_when_paused":bool, "trigger_events":["Pressed"|"Released" with linked exec] }`. Existing Enhanced Input entries unchanged; the early return only applies when neither class family exists.
- Unsupported-node rule: any `UK2Node` subclass, `EdGraphNode_Comment` and material graph nodes are supported. `unsupported_node_types` and the confidence penalty only count other classes.
- `has_tick` (per graph and in `complexity_metrics`) is true only for a **live** Tick event (D4 definition). Raw node counts in `complexity_metrics` stay unchanged; reachability metrics live in `graph_metrics`.

### D3 blueprint.list
- Params: `path` (unchanged), `type` (string), `exclude_types` (array of strings). Case-insensitive; unknown values → `INVALID_FIELD` listing the vocabulary. `type` and `exclude_types` may combine.
- Vocabulary (one shared classifier for AssetData and loaded BP): `Interface`, `FunctionLibrary`, `MacroLibrary` (BlueprintType tag), `AnimBlueprint` (asset class AnimBlueprint or child), `EditorUtility` (asset class EditorUtilityBlueprint/EditorUtilityWidgetBlueprint or native parent child of an editor-utility base), `Widget`, `Component`, `Actor`, `Object` (any other resolved UObject parent), `Unknown` (parent unresolvable). Precedence in this order.
- Rows add `native_parent_class` (short name of NativeParentClassPath) and `asset_class`; `parent_class` becomes the immediate parent (ParentClassPath; for a BP generated class, the Blueprint asset name without `_C`) and `parent_class_path` its object path.
- `create` keeps its five creatable types; get_info/analyze report the new vocabulary.

### D4 blueprint.graph_metrics (new, read-only)
Request `{asset_path, thresholds?: {max_nodes_per_entry: int=10, max_nodes_per_blueprint: int=30}}`.
Definitions:
- Entry node: node with ≥1 exec output pin and no exec input pin (events, custom/bound/input events, Enhanced Input and debug-key nodes, function entries) in ubergraph and function graphs.
- Live entry: function entries always; other entries iff an exec output is linked.
- Reachable set of an entry: exec-forward BFS from the entry, then backward data closure over input pins (shared helper extracted from CortexBPGraphCleanupOps, also used by delete_orphaned_nodes).
- Functional node: reachable, not the entry itself, not `UK2Node_FunctionResult`, comment, or `K2Node_Knot`. Macro instances and collapsed (composite) nodes are replaced by the functional-node count of their inner graph (recursively, cycle-guarded).
- Conditionals: `UK2Node_IfThenElse`, any `UK2Node_Switch` subclass, `UK2Node_Select`, impure `UK2Node_DynamicCast`, plus conditionals inside expanded macros/composites.
- Getter fan-out: for each reachable `UK2Node_VariableGet`, number of distinct linked consumer nodes; report `max_getter_fanout` and `variable_gets`.
Response:
```
{ asset_path, has_tick, totals:{functional_nodes, unreachable_nodes, ghost_entries, entries},
  entries:[{graph, name, kind:"event"|"function"|"input"|"custom_event"|"bound_event", node_id, live,
            functional_nodes, conditionals, variable_gets, max_getter_fanout}],
  unreachable:[{graph, count, node_ids:[...≤50]}],
  ghost_entries:[{graph, name, node_id}],
  thresholds:{...}, violations:[{scope:"entry"|"blueprint", graph?, name?, functional_nodes, limit}] }
```
Ghost (non-live) entries report `functional_nodes:0`. `has_tick` true iff a live ReceiveTick entry exists.

### D5 graph.search_nodes inventory
- `graph_name` restricts to that graph (with `subgraph_path` as today).
- All filters optional; no filter returns every node (compact rows) of the selected scope. Python already pages `results` via cursor (`CACHED_PAGE_ARRAYS`), giving the paged inventory.
- `node_class`: case-insensitive; matches the node class or any superclass name (leading `U` optional) — `K2Node` returns all K2 nodes; a trailing `*` means prefix match on the runtime class name.

### D6 data.get_struct_schema
Accept short name, `F`-toggled name, or object path. Factor CortexDataSchemaExportOps' `ResolveStructByName` into a shared CortexData private helper; for `/Game/...` paths load the user struct after `FPackageName::DoesPackageExist`.

### D7 analyze_for_migration `sections`
Optional `sections: array[string]`; omitted/empty = all. Unknown names → `INVALID_FIELD` before any output. Prerequisite sections are computed internally and only selected sections are emitted (identity fields `name, asset_path, type, parent_class, parent_class_path` always present). Python adds a complete-or-refuse policy for `blueprint.analyze_for_migration` suggesting `sections`.

## Contracts and synchronisation
New/changed declarations in handlers; `MCP/tests/fixtures/capabilities_cache_full.json` updated and `_fallback_generated.py` regenerated; Python wrapper docstrings for list/search/get_struct_schema/analyze updated; toolkit `resources/mcp-tool-reference.md` synced in the #74 toolkit PR. All new operations are read-only, game-thread routed, no transactions.
