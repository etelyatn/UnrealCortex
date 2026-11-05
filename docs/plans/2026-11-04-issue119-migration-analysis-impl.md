# Issue 119 Migration Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make analyze_for_migration, blueprint.list, graph.search_nodes and data.get_struct_schema accurate on UE 5.8 cold-loaded assets, and add reachability-aware `blueprint.graph_metrics`.

**Architecture:** Fixes stay in their owning domain ops (CortexBlueprint, CortexGraph, CortexData). Two private shared helpers are introduced in CortexBlueprint: widget enumeration (UMG dependency) and exec/data reachability (extracted from CortexBPGraphCleanupOps). Python facade only gains policy/docstring/fixture updates.

**Tech Stack:** UE 5.8 C++, Unreal automation tests, Python/uv/pytest MCP server.

**Spec:** [Design](2026-11-04-issue119-migration-analysis-design.md) — exact field names, vocabularies and definitions (D1–D7) are there; read it before each task.

## Global Constraints

- Plugin base 1ac0646fed0448103fa0e56f003fb61ff80e02a1, branch `fix/issue119-migration-analysis` in `Plugins/UnrealCortex`. Never stage `MCP/pyproject.toml`, `MCP/uv.lock`, `UnrealCortex.uplugin` (pre-existing user edits).
- All shell commands through `rtk`. Native git: `rtk proxy powershell.exe -NoProfile -Command "git -C Plugins/UnrealCortex ..."`. No worktrees, reset, clean, stash, push.
- Build (full target, from CortexSandbox root; no CortexSandbox editor may be running): `rtk proxy powershell.exe -NoProfile -Command "& 'D:/UnrealEngine/UE_5.8/Engine/Build/BatchFiles/Build.bat' CortexSandboxEditor Win64 Development -Project='D:/UnrealProjects/CortexSandbox/CortexSandbox.uproject' -WaitMutex -FromMsBuild"`. Expect `Result: Succeeded`. Never trust tests after a failed build. A user-owned RipperMirror editor is running: never terminate it.
- TDD: failing test observed before production code. Zero warnings in tests; skip conditions use AddInfo. Test assets under `/Game/Temp/Cortex119/...`, cleaned up (MarkAsGarbage; delete saved packages).
- No game-specific names/types in plugin code. Read-only commands: no transactions, no saves. Game thread only.
- Test naming: `Cortex.Blueprint.<Area>.<Case>`, `Cortex.Graph.<Area>.<Case>`, `Cortex.Data.<Area>.<Case>`.
- Implementers never dispatch subagents and never push.

## Review Focus

1. A WBP whose widgets live in a NamedSlot or nested user widget must still be enumerated (ForEachWidget, not RootWidget-only walk) — Task 1 test includes a named-slot child.
2. A function graph whose entry is unlinked (empty function) must count as live with 0 functional nodes, not as a ghost — Task 4 test.
3. Macro instance recursion (macro containing itself indirectly) must not hang — Task 4 cycle guard test via composite nesting.
4. `blueprint.list type:"Widget"` must exclude EditorUtilityWidget Blueprints per D3 precedence, and `exclude_types` combined with `type` must not invert — Task 3 test.
5. `search_nodes` without filters on a large graph must stay complete through Python cursor paging (no silent truncation) — Task 5 Python test.

---

### Task 1: Widget enumeration after cold load (D1)

**Files:** Create `Source/CortexBlueprint/Private/Operations/CortexBPWidgetUtils.h/.cpp`; modify `CortexBlueprint.Build.cs` (private `UMG`), `CortexBPAnalysisOps.cpp` (`ExtractWidgetEntities`), `CortexBPToolbarExtension.cpp` (~156); test `Source/CortexBlueprint/Private/Tests/CortexBPAnalysisWidgetReloadTest.cpp`.

**Interfaces:** Produces `namespace CortexBPWidgetUtils { TArray<UWidget*> CollectWidgets(const UBlueprint* WidgetBlueprint); }` (empty for non-widget BP).

- [ ] Write `Cortex.Blueprint.Analysis.WidgetEntitiesAfterReload`: create WBP via handler `create` type Widget under `/Game/Temp/Cortex119/`, build tree (CanvasPanel root → TextBlock, Button, NamedSlot with a child), compile, `UPackage::SavePackage`, `ResetLoaders`+`MarkAsGarbage`+`CollectGarbage`, then `analyze_for_migration`: assert `widgets` names == all 5 widgets and `widget_dependencies` non-empty for the widget classes; cleanup deletes the package file. Pattern: CortexBPAnalysisTest.cpp:954-1085.
- [ ] Build+run; expect FAIL (widgets empty).
- [ ] Implement helper + both call sites; no `AllWidgets` reader remains (grep).
- [ ] Build+run `Cortex.Blueprint+`; PASS, zero warnings. Commit `fix(blueprint): enumerate widget trees after cold load (#119)`.

### Task 2: analyze_for_migration output defects + unsupported rule + live has_tick (D2)

**Files:** `CortexBPAnalysisOps.cpp`; test `Source/CortexBlueprint/Private/Tests/CortexBPAnalysisOutputFixesTest.cpp`. Uses Task 4's liveness rule — implement a local `IsLiveEntry(const UEdGraphNode*)` (exec output linked) in a header `Operations/CortexBPGraphReachability.h` that Task 4 extends.

**Interfaces:** Produces `namespace CortexBPGraphReachability { bool IsEntryNode(const UEdGraphNode*); bool IsLiveEntry(const UEdGraphNode*); }`.

- [ ] Tests (one per D2 bullet): `Cortex.Blueprint.Analysis.EventsExcludeNone` (custom event + component-bound event on an SCS component → no `"None"`, bound event as `Comp.Delegate`), `.FunctionOutputsDeduplicated` (two result nodes), `.InterfaceEventImplementations` (BP implementing a BPI/native interface event without outputs, event placed in ubergraph → listed), `.BoolDefaultFromCDO` (untouched bool member → `"false"`; bool local → `"false"`), `.InputDebugKeyBinding` (skip with AddInfo if InputBlueprintNodes absent), `.K2NodeSubclassSupported` (node of a UK2Node subclass not prefixed `K2Node_`, e.g. a GameplayTags K2 switch node if loadable, else AddInfo skip + any UK2Node subclass found by iterating classes whose name lacks the prefix), `.GhostTickNotCounted` (actor BP with unconnected ReceiveTick → `has_tick:false` per graph and in complexity_metrics; linked Tick → true).
- [ ] Observe failures; implement; PASS on `Cortex.Blueprint+`. Update existing assertions only where they pinned the defect. Commit `fix(blueprint): correct migration analysis output fields (#119)`.

### Task 3: blueprint.list filters and parents; unified classifier (D3)

**Files:** `CortexBPAssetOps.cpp` (`List`, `DetermineBlueprintType`, `DetermineTypeFromAssetData` → one shared classifier), `CortexBPCommandHandler.cpp` (declare `type`, `exclude_types`); tests `CortexBPListAndInfoTest.cpp` / `CortexBPTypeDetectionTest.cpp`.

- [ ] Tests: `Cortex.Blueprint.List.FilterType` (Actor + Widget + Interface BPs in a temp folder; `type:"widget"` returns only the WBP; invalid type → INVALID_FIELD), `.ExcludeTypes`, `.ParentClassBlueprint` (BP child of BP → `parent_class` = parent BP name, `native_parent_class` = Actor), `Cortex.Blueprint.TypeDetection.ExtendedVocabulary` (AnimBlueprint when creatable, plain UObject parent → `Object`, EditorUtility when the class exists; AddInfo skips otherwise).
- [ ] Implement; PASS; commit `fix(blueprint): honour list type filters and report parent chain (#119)`.

### Task 4: blueprint.graph_metrics (D4)

**Files:** `Operations/CortexBPGraphReachability.h/.cpp` (extract `GatherReachableExecNodesFromEvents`/`ExpandToLinkedDataDependencies` from CortexBPGraphCleanupOps.cpp; cleanup uses it), new `Operations/CortexBPGraphMetricsOps.h/.cpp` `static FCortexCommandResult GraphMetrics(const TSharedPtr<FJsonObject>& Params)`, handler dispatch + read-only list + declaration; test `Tests/CortexBPGraphMetricsTest.cpp`.

- [ ] Tests: `Cortex.Blueprint.GraphMetrics.GhostTick` (default actor BP: ghost Tick/BeginPlay → `has_tick:false`, those entries `live:false, functional_nodes:0`), `.LiveEntryCounts` (BeginPlay → Branch → 2 PrintString with a getter feeding both + disconnected node: functional_nodes 3+getter, conditionals 1, max_getter_fanout 2, unreachable count 1), `.EmptyFunctionLive`, `.MacroAndCompositeExpanded` (collapsed graph with 2 nodes counts 2, cycle guard), `.Violations` (thresholds `{max_nodes_per_entry:2}` → entry violation), `.InvalidPath`. Existing `Cortex.Blueprint.GraphCleanup+` must still pass.
- [ ] Implement; PASS `Cortex.Blueprint+`; commit `feat(blueprint): add reachability-aware graph_metrics (#119)`.

### Task 5: search_nodes inventory + get_struct_schema paths (D5, D6)

**Files:** `Source/CortexGraph/Private/Operations/CortexGraphNodeOps.cpp` (`SearchNodes`), `CortexGraphCommandHandler.cpp` (descriptions), graph search test file; `Source/CortexData/Private/Operations/CortexDataTableOps.cpp` (`GetStructSchema`), shared resolver extracted from `CortexDataSchemaExportOps.cpp`; data test.

- [ ] Tests: `Cortex.Graph.SearchNodes.GraphNameScope`, `.NoFilterInventory`, `.SubclassMatch` (`K2Node` matches all, `K2Node_Call*` prefix), `Cortex.Data.GetStructSchema.FullPath` (`/Script/Engine.Vector`... use a USTRUCT with properties, e.g. `/Script/Engine.HitResult`, and a sandbox user-defined struct path if present).
- [ ] Implement; PASS `Cortex.Graph+` then `Cortex.Data+`; commit `fix(graph,data): paged node inventory and struct path resolution (#119)`.

### Task 6: sections selector + MCP contract sync (D7)

**Files:** `CortexBPAnalysisOps.cpp`, `CortexBPCommandHandler.cpp`; `MCP/src/cortex_mcp/response.py` (`_COMPLETE_OR_REFUSE_POLICIES["blueprint.analyze_for_migration"]`), `MCP/src/tools/blueprint/{analysis,assets}.py`, `MCP/src/tools/graph/*search*`, `MCP/src/tools/data/datatables.py` docstrings/params, `MCP/tests/fixtures/capabilities_cache_full.json` + regenerated `_fallback_generated.py` (`uv run python scripts/sync_fallback.py --from-fixture`), `mcp_tool_signature_snapshot.json` if wrapper signatures change; Python tests in `MCP/tests/test_blueprint_analysis.py`, `test_response.py`.

- [ ] Native tests `Cortex.Blueprint.AnalyzeForMigration.SectionsSelector` / `.UnknownSectionRefused`; Python tests for refuse policy and for unfiltered search_nodes paging completeness.
- [ ] Implement; PASS `Cortex.Blueprint+`, Python non-e2e suite, `sync_fallback.py --check`. Commit `feat(blueprint): analyze sections selector and contract sync (#119)`.

### Acceptance (controller)
Full build; serial `Cortex.Blueprint+`, `Cortex.Graph+`, `Cortex.Data+`, `Cortex.Core+`; Python staged non-e2e; live sandbox editor smoke through MCP routes (list type, graph_metrics, analyze on a saved WBP, search_nodes paging); Ripper live check per design; verification report `docs/verification/2026-11-04-issue119-migration-analysis.md`.
