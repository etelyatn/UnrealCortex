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
CortexBlueprint gains private dependency `UMG` (CortexGraph already depends on UMG; no domain coupling). One private helper `CortexBPWidgetUtils::CollectWidgets(const UBlueprint*) -> TArray<UWidget*>` reads the `WidgetTree` object property by reflection (WBP class stays in UMGEditor, not linked), casts to `UWidgetTree` and calls `ForEachWidget` (includes named-slot content). Output order: tree pre-order (deterministic). Both `ExtractWidgetEntities` and the toolbar extension use it. No reader of `AllWidgets` remains. **Full tree:** `analyze_for_migration.widgets` emits EVERY widget of the tree (matching `umg.get_tree`), not only variable or event-bound ones; each row keeps `is_variable` and `bound_events_in_graph` so consumers can filter. `entity_summary` deliberately keeps its previous semantic for widgets: only variable widgets and widgets with graph-bound events (the entities a C++ migration must bind). `widget_dependencies` reads `entry_widget_class` from all rows (non-variable ListViews are now included).

### D2 analyze_for_migration output fixes
- `events`: never emit a `None` member name. `UK2Node_ComponentBoundEvent` emits `"<ComponentPropertyName>.<DelegatePropertyName>"`. Custom events stay only in `custom_events`.
- Function `outputs`: union of result-node user pins, deduplicated by pin name, first-seen order.
- `interfaces_implemented[].functions`: function-graph implementations plus ubergraph `UK2Node_Event` nodes whose `EventReference` parent class is the interface (or child). Entries stay strings (member name), deduplicated.
- `default_value`: a member variable with a non-empty description default reports that string unchanged. With an empty description default it reports the textual value of the property on the generated class CDO: bool → `"true"`/`"false"`, otherwise `FProperty::ExportText_Direct` with the value passed as its own delta. `ExportText_Direct` (Property.cpp) takes its `Data == Delta` branch and always exports, whereas a null delta would compare against the type default and export nothing for `0`/`""`/`None` values. Pinned engine output for untouched members: int `"0"`, FString `""`, FVector `"(X=0.000000,Y=0.000000,Z=0.000000)"`, object reference `"None"`. **Semantic change:** untouched non-bool members previously reported `""`; they now report the CDO value. Local variable with empty default and bool type → `"false"`; other empty locals unchanged (type default, as the editor shows).
- `input_bindings`: add `UK2Node_InputDebugKey` (lazy class lookup `/Script/InputBlueprintNodes.K2Node_InputDebugKey`, reflection reads) emitting `{ "kind":"debug_key", "key":<FKey name>, "modifiers":{ "ctrl","shift","alt","cmd" }, "execute_when_paused":bool, "trigger_events":["Pressed"|"Released" with linked exec] }`. Existing Enhanced Input entries unchanged; the early return only applies when neither class family exists.
- Unsupported-node rule: any `UK2Node` subclass, `EdGraphNode_Comment` and material graph nodes are supported. `unsupported_node_types` and the confidence penalty only count other classes.
- `has_tick` (per graph and in `complexity_metrics`) is true only for a **live** Tick event (D4 definition). Raw node counts in `complexity_metrics` stay unchanged; reachability metrics live in `graph_metrics`.

### D3 blueprint.list
- Params: `path` (unchanged), `type` (string), `exclude_types` (array of strings). Case-insensitive; unknown values → `INVALID_FIELD` listing the vocabulary. `type` and `exclude_types` may combine.
- Vocabulary (one shared classifier for AssetData and loaded BP): `Interface`, `FunctionLibrary`, `MacroLibrary` (BlueprintType tag), `AnimBlueprint` (asset class AnimBlueprint or child), `EditorUtility` (asset class EditorUtilityBlueprint/EditorUtilityWidgetBlueprint or native parent child of an editor-utility base), `Widget`, `Component`, `Actor`, `Object` (any other resolved UObject parent), `Unknown` (parent unresolvable). Precedence in this order.
- Rows add `native_parent_class` (short name of NativeParentClassPath) and `asset_class`; `parent_class` becomes the immediate parent (ParentClassPath) and `parent_class_path` its object path. An Asset Registry tag value of `None` (or empty) yields empty `native_parent_class`/`parent_class_path`/`parent_class` (a missing native tag falls back to the parent tag and vice versa).
- **Unified identity across commands.** `list`, `get_info`, `analyze_for_migration` and `create` share `FCortexBPAssetOps::DetermineBlueprintType` (type) and `ParentClassDisplayName` / `NativeParentClassName` (parents). `type` is always from the vocabulary above: analyze no longer emits `"WidgetBlueprint"` (Widget Blueprints, including EditorUtilityWidget Blueprints, report `Widget`/`EditorUtility` like list). `parent_class` is the immediate parent display name: the class name, with `_C` stripped only for Blueprint generated classes outside `/Script/` (a BP child reports its parent Blueprint asset name, a native parent reports e.g. `Actor`). `native_parent_class` is the short name of the nearest native ancestor; get_info and analyze add it. `parent_class_path` stays the exact class object path (analyze, list). `create` reports `parent_class` through the same helper.
- `create` keeps its five creatable types; get_info/analyze report the new vocabulary.

### D4 blueprint.graph_metrics (new, read-only)
Request `{asset_path, thresholds?: {max_nodes_per_entry: int=10, max_nodes_per_blueprint: int=30}}`.
Definitions:
- Graph scope: every `UbergraphPages` and `FunctionGraphs` graph plus every `ImplementedInterfaces[i].Graphs` graph (interface function implementations, entry kind `function`, name = graph name). Only graphs whose schema `IsChildOf(UEdGraphSchema_K2)` are measured (this includes `UWidgetGraphSchema`, used by Widget Blueprint function/event graphs), excluding graphs whose `Schema->GetGraphType(Graph)` is `GT_Animation` or `GT_StateMachine` (pose-flow networks without event/exec semantics; asked of the schema, so no animation-module dependency). AnimGraphs and state machine graphs produce no entries and no `unreachable` rows. Macro graphs (`MacroGraphs`) are never measured on their own; they count only where a macro instance expands them (below). Entry node: node with ≥1 exec output pin and no exec input pin (events, custom/bound/input events, Enhanced Input and debug-key nodes, function entries) in those graphs.
- Live entry: function entries always; other entries iff an exec output is linked.
- Reachable set of an entry: exec-forward BFS from the entry, then backward data closure over input pins (shared helper extracted from CortexBPGraphCleanupOps, also used by delete_orphaned_nodes). For function entries the closure is additionally seeded from every `UK2Node_FunctionResult` in the same graph, so pure nodes feeding a result with no exec wire from the entry are counted as logic and not reported unreachable.
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
Accept short name, `F`-toggled name, or object path. Factor CortexDataSchemaExportOps' `ResolveStructByName` into a shared CortexData private helper; for object paths under **any mounted package root** (`/Game`, plugin and engine content) load the struct only after `FPackageName::IsValidLongPackageName` (read-only roots allowed), a mounted-root check (`GetPackageMountPoint`), `FPackageName::DoesPackageExist` and an Asset Registry class check that the asset is a `UScriptStruct`; non-struct assets are refused without loading.

### D7 analyze_for_migration `sections`
Optional `sections: array[string]`; omitted/empty = all. Unknown names → `INVALID_FIELD` before any output. Prerequisite sections are computed internally and only selected sections are emitted (identity fields `name, asset_path, type, parent_class, native_parent_class, parent_class_path` always present). Python adds a complete-or-refuse policy for `blueprint.analyze_for_migration` suggesting `sections`, and likewise for `blueprint.graph_metrics` (suggesting `analyze_for_migration` with `sections=['complexity_metrics']` or a smaller Blueprint).

## Contracts and synchronisation
New/changed declarations in handlers; `MCP/tests/fixtures/capabilities_cache_full.json` updated and `_fallback_generated.py` regenerated; Python wrapper docstrings for list/search/get_struct_schema/analyze updated; toolkit `resources/mcp-tool-reference.md` synced in the #74 toolkit PR. All new operations are read-only, game-thread routed, no transactions.
