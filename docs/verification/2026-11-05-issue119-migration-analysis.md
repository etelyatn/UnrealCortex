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
| `get_struct_schema` full object paths | PASS | `Cortex.Data.GetStructSchema.{FullPath,NonStructAssetRefused,MountedRoot}`; live `/Script/Engine.HitResult` |
| Optional `sections` with complete-or-refuse | PASS | `Cortex.Blueprint.AnalyzeForMigration.{SectionsSelector,UnknownSectionRefused}`; pytest router tests; live sectioned reads |

## Runs

- Controller: `Cortex.Blueprint+` 290/290 @`6ff5688d` and 291/291 @`0638aaa1` (`AutomationTest_2026-11-05_085330.log`; one environment warning: engine `LogHttp` 3 s timeout probing `www.google.com/generate_204`, not plugin code); `Cortex.Core+` 205/205; Python non-e2e 1111 passed; `sync_fallback --check` OK.
- Implementers: `Cortex.Graph+` 324/324, `Cortex.Data+` 134/134 @`e2c4bf85` (no later Graph/Data changes); `Cortex.Blueprint+` 291/291 @`0638aaa1`.
- Base refresh: `origin/main` advanced to `d445f99c` (#123 widget migration, #124 migration verification) during delivery and was merged into the branch. Conflicts: Blueprint read-only command list (union), `FCortexBPAssetOps` declarations (union), capabilities fixture (both sides' disjoint command entries) and regenerated fallback. `compile_all.types` now uses the shared Blueprint type vocabulary (`FCortexBPAssetOps::GetBlueprintTypeNames`) so it cannot drift from `list`. Merged-tree controller runs (`AutomationTest_2026-11-05_091107` … `091540`): `Cortex.Blueprint+` 299/299, `Cortex.Graph+` 328/328, `Cortex.Data+` 134/134, `Cortex.Core+` 207/207, `Cortex.UMG+` 172/172, zero warnings; Python non-e2e 1119 passed; `sync_fallback --check` OK.
- Environment: intermittent full-suite editor crash (60 s Blueprint-compile stall, then `Background Worker crashed`) while another editor was running; confirmed by the maintainer as a known environment flake; affected runs were rerun clean.

## Limitations

- Positive load of a non-`/Game` user-defined struct is untested (no such asset in the sandbox); refusal paths are tested.
- `graph_metrics` measures K2-derived graphs only (animation and state-machine graphs skipped); macro graphs are counted at instance sites.

## Post-merge hardening

Source: final review of merge `b2b3ea2c` (PR #190) — design `docs/plans/2026-11-04-issue119-migration-analysis-design.md` ("Post-merge hardening" and the amended D2-D5). Engine facts used: `Kismet2.cpp:716-741` (`AddDefaultEventNode` links a ghost event's `then` to an auto-placed `CallParentFunction`, both `MakeAutomaticallyPlacedGhostNode` → `EnabledState` Disabled, `EdGraphNode.cpp:1132`), `EdGraphPin.cpp:1976` (linking through the schema converts connected ghosts to real nodes), `EdGraphNode.h:410` (`IsNodeEnabled`).

| Finding | Change | Test (RED before the fix, GREEN after) |
|---|---|---|
| I1 disabled/ghost nodes counted live | `IsLiveEntry` requires `IsNodeEnabled()`; exec BFS and data closure take `bEnabledOnly` (metrics callers only; `delete_orphaned_nodes` unchanged); disabled nodes never functional; `disabled_nodes` + `disabled` rows | `GraphMetrics.GhostEventLinkedToDisabledParentCall`, `GraphMetrics.DisabledNodeInLiveChain`, `Analysis.GhostTickLinkedToDisabledParentCall` |
| I2 macro/composite counted wholesale | inner graphs seeded from entry tunnels (pure graphs from exit tunnels), same enabled-only closure; orphans in `unreachable` rows with `subgraph` | `GraphMetrics.CompositeInnerReachability`, `GraphMetrics.MacroAndCompositeExpanded` (rewired through entry tunnels) |
| I3 AnimGraph nodes "supported" | K2 nodes supported unless in a `GT_Animation`/`GT_StateMachine` graph (shared `IsAnimationOrStateMachineGraph`) | `Analysis.AnimGraphNodesStayUnsupported` |
| 4 extra return node seeds closure | seed only pure functions / sole unwired result | `GraphMetrics.UnwiredSecondResultUnreachable` |
| 5 unresolved macro counted 0 | counts as one functional node | `GraphMetrics.CompositeInnerReachability` |
| 6 frontend `"WidgetBlueprint"` literal | `"type":"Widget"` matched; fixtures use real shapes (`CommonActivatableWidget` parent) | `Frontend.Conversion.Widget.FragmentSelection` |
| 7 `list` empty/null `type` | treated as absent | `List.FilterType` |
| 9 interface events parent/child | exact declaring interface only | `Analysis.InterfaceEventExactParent` (child interface derived from the parent interface class) |
| 10 `thresholds` | unknown keys → `INVALID_FIELD`; null → absent | `GraphMetrics.ThresholdKeyValidation` |
| 11 `node_class` prefix | `U` never stripped from `*` prefixes | `SearchNodes.SubclassMatch` (`U*`, `UK2Node*`) |
| 12 test hygiene | registry notification (`AssetDeleted` while loaded; folder rescan after file removal), per-run folders removed when empty, residue asserted | `Analysis.WidgetEntitiesAfterReload`, `Analysis.WidgetDependencies` |
| 14 docstring | standalone `graph_search_nodes` no longer promises paging | `pytest` (no docstring assertions) |
| 8 locals `default_value` | documented only (no code change) | — |

RED evidence: with production files reverted to `b2b3ea2c` and the new tests in place, `GraphMetrics` failed 5/16 (CompositeInnerReachability, DisabledNodeInLiveChain, GhostEventLinkedToDisabledParentCall, ThresholdKeyValidation, UnwiredSecondResultUnreachable), `Analysis` 2/29 (AnimGraphNodesStayUnsupported, GhostTickLinkedToDisabledParentCall), `List` 1/5 (FilterType), `Graph.SearchNodes` 1/5 (SubclassMatch), `Frontend.Conversion.Widget` 1/9 (FragmentSelection), and a stale build with the pre-fix `AnalysisOps` failed `Analysis.InterfaceEventExactParent`. GREEN after restoring the production files: `Cortex.Blueprint+` 307/307, `Cortex.Graph+` 328/328, `Cortex.Frontend.Conversion+` 52/52 (`Cortex.Data` untouched); Python non-e2e 1119 passed. Not re-run: live Ripper acceptance (review item 13) — the live checks of `referenced_by_widgets` on the merged head remain open.

## Slow-model review follow-up

Source: independent post-merge review of PR #190/#191 (merge `d8003fa1`), three `graph_metrics` findings, each confirmed against the source and UE 5.8 engine source before fixing (design: `docs/plans/2026-11-04-issue119-migration-analysis-design.md`, "Slow-model review follow-up"). The hardening table above describes the state before this follow-up: its `bEnabledOnly` parameters and entry-tunnel-wholesale seeding are superseded by the pin-aware reach (`CortexBPGraphExecReach`).

| Finding | Confirmed by | Change | Test |
|---|---|---|---|
| P1 disabled node passthrough | `EdGraphUtilities.cpp:302-331` relinks incoming exec links to `GetPassThroughPin` before removing a disabled node; `K2Node.cpp:2045-2090` supplies it for one exec in / one exec out | Disabled nodes are traversed through the passthrough, stay non-functional and reported as disabled; disabled multi-output nodes block; disabled entries stay non-live | `GraphMetrics.DisabledNodeInLiveChain` (corrected expectation: A, feeder, C count, nothing unreachable; two chained disabled nodes), `GraphMetrics.DisabledMultiOutputBlocks` |
| P2 tunnel pin routing | design D4; `EdGraphSchema_K2.cpp:7108-7152` matches tunnel pins by name and opposite direction | Entered instance pin → entry-tunnel pin of the same name; only reached exit-tunnel pins continue; one context per instance, merged per graph for totals and findings | `GraphMetrics.TunnelPinRouting` (two-input composite, 1-node and 11-node routes, unused input, distinct outputs) |
| P2 getters across expansion | design D4 | Getters and fan-out measured in every expanded context; boundary edges translated | `GraphMetrics.GettersAcrossExpansion` |

RED (production files restored to `31c336f0^`, new tests in place, gated build `Result: Succeeded`, `Cortex.Blueprint.GraphMetrics` 17/20): `DisabledNodeInLiveChain` (functional 2 ≠ 3, unreachable 1 ≠ 0), `GettersAcrossExpansion` (`variable_gets` 0 ≠ 1, fan-out 0 ≠ 2 and 1 ≠ 2 across the boundary), `TunnelPinRouting` (BeginPlay 14 ≠ 2, route-B rows 0 ≠ 11). `DisabledMultiOutputBlocks` passes before and after (guard against over-traversal). GREEN: `Cortex.Blueprint.GraphMetrics` 20/20, `Cortex.Blueprint.GraphCleanup` 4/4 (`delete_orphaned_nodes` unchanged). Existing macro/composite fixtures were rewired with name-matched entry/exit tunnel pins and exits (the previous fixtures had no exit, which real composites always have).

Final gate at the follow-up head (gated build `Result: Succeeded`): `Cortex.Blueprint` 319/319 (`AutomationTest_2026-11-05_155339`, one unrelated `LogHttp` network-timeout warning), `Cortex.Reflect` 91/91 (`155517`), `Cortex.Core` 207/207 (`155554`); Python non-e2e 1155 passed; `sync_fallback.py --from-fixture --check` OK (no declaration changed). Not re-run: live Ripper acceptance.
