# Issue 120 Dependency Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Project-wide dependency knowledge in few calls: config-aware referencers, broken config paths, one-call migration-analysis export, typed dependency graph with wave order, by-name member access detection, duplicate graph detection, batch_query pagination, durable cursors.

**Architecture:** Cross-asset analysis lives in CortexReflect (config scanner, dependency graph, name lookups, duplicate graphs; private `UMG` dep added, no domain coupling). The analysis export lives in CortexBlueprint next to analyze/graph_metrics and reuses their builders. Pagination is Python-owned.

**Tech Stack:** UE 5.8 C++, Unreal automation, Python/uv/pytest MCP server.

**Spec:** [Design](2026-11-05-issue120-dependency-map-design.md) (D1–D7 binding).

## Global Constraints
- Plugin base b2b3ea2c, branch `feat/issue120-dependency-map` in `Plugins/UnrealCortex`. Never stage `MCP/pyproject.toml`, `MCP/uv.lock`, `UnrealCortex.uplugin`; no `uv lock`/`uv add`.
- All shell via `rtk`; native git through `rtk proxy powershell.exe -NoProfile -Command "git -C Plugins/UnrealCortex ..."`. No worktrees/reset/clean/stash/push.
- Build + tests: `rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:/UnrealProjects/CortexSandbox/.superpowers/run119.ps1 <filters...>` (builds CortexSandboxEditor, runs tests only if `Result: Succeeded`). One run at a time. Known environment flake: 60 s compile stall then "Background Worker crashed" — rerun once, record the log.
- Python: from `Plugins/UnrealCortex/MCP` via PowerShell: `uv run pytest tests/ -m "not e2e and not scenario and not stress" -q -p no:cacheprovider --basetemp=D:/UnrealProjects/CortexSandbox/Saved/_pytest120`.
- TDD (RED observed before production code), zero warnings, AddInfo skips, test-owned files/assets removed. Read-only commands: no transactions, no saves; file outputs only through `FCortexSafeFileContract`. Game thread only. No game-specific names. `LogCortexReflect` / `LogCortexBlueprint`.
- New commands declared in handlers; capabilities fixture mirrors declarations exactly; `_fallback_generated.py` regenerated (`--check` clean) in the last task.
- Test names: `Cortex.Reflect.<Area>.<Case>`, `Cortex.Blueprint.<Area>.<Case>`.

## Review Focus
1. Config tokens embedded in struct literals (`(Path="/Game/X")`, `+Maps=/Game/Maps/M`, object paths with `_C`, quoted with trailing comma) must normalise to the right package — Task 1 test lines.
2. A directory path that exists on disk but holds no assets yet must not be reported broken — Task 1.
3. Dependency cycles (A↔B inheritance impossible, but casts/widget nesting cycles are common) must not drop nodes from the wave output — Task 3 cycle test.
4. Cursor pages for a batch must never re-execute the batch (mutating steps) — Task 6 test counts sends.
5. Export over a prefix containing an unloadable/corrupt or blocked Blueprint must still write the other rows and report the failure — Task 2.

---

### Task 1: Config scanner, config referencers, find_broken_config_paths (D1)
Files: `Source/CortexReflect/Private/Operations/CortexReflectConfigScan.h/.cpp` (`namespace CortexReflectConfigScan { struct FConfigReference { FString File; int32 Line; FString Section; FString Key; FString Value; FString Path; FString PackageName; bool bDirectory; }; TArray<FConfigReference> Scan(); }`), `CortexReflectOps.cpp` (`GetReferencers` + new `FindBrokenConfigPaths`), `CortexReflectCommandHandler.cpp`; test `Private/Tests/CortexReflectConfigTest.cpp`.
- [ ] Tests: `Cortex.Reflect.Config.ScanFormats` (test writes `Config/CortexTest120.ini` with `[/Script/Test.Settings]` lines covering `+Items=/Game/Blueprints/BP_SimpleActor.BP_SimpleActor_C`, `Map=(Path="/Game/X/Y")`, `Dir=/Game/Missing120/`, comments; deletes it after), `.ReferencersIncludeConfig` (get_referencers on `/Game/Blueprints/BP_SimpleActor` returns a `type:"config"` row with file/line/section/key; `include_config:false` omits it), `.BrokenPaths` (missing package and missing directory reported; existing package and existing empty disk directory not reported).
- [ ] Implement; `Cortex.Reflect+` GREEN; commit `feat(reflect): config-aware referencers and broken config paths (#120)`.

### Task 2: blueprint.export_migration_analysis (D2)
Files: `CortexBPAnalysisOps.h/.cpp` (extract `static TSharedPtr<FJsonObject> BuildMigrationAnalysis(UBlueprint*, const TSet<FString>& Sections, FString& OutError)` used by `AnalyzeForMigration`), `CortexBPGraphMetricsOps.h/.cpp` (extract `static TSharedPtr<FJsonObject> BuildGraphMetrics(const UBlueprint*, const FThresholds&)`), new `Operations/CortexBPMigrationExportOps.h/.cpp`, handler; tests `Private/Tests/CortexBPMigrationExportTest.cpp`.
- [ ] Tests: `Cortex.Blueprint.MigrationExport.Prefix` (temp folder with Actor BP, child BP of it, WBP → JSONL 3 rows identical to single-call analyze output for the same sections; index rows with parent_chain for the child; totals), `.TypesAndSections`, `.Refusals` (both/neither of path_prefix/paths, bad type, bad section, out_path outside allowed roots → no file written), `.MissingPathRow`.
- [ ] Implement; `Cortex.Blueprint+` GREEN (existing analyze/graph_metrics tests unchanged); commit `feat(blueprint): one-call migration analysis export (#120)`.

### Task 3: reflect.export_dependency_graph (D3)
Files: new `Operations/CortexReflectDependencyGraphOps.h/.cpp`, CortexReflect.Build.cs (private `UMG`), handler; test `Private/Tests/CortexReflectDependencyGraphTest.cpp`.
- [ ] Tests: fixture under `/Game/Temp/Cortex120/Graph/` built in test: BPI + implementer + caller (Message node), base BP + child (inherits), cast, spawn, CreateWidget of a WBP, WBP containing a child WBP and a ListView with entry class, struct/enum use if a sandbox UDS/UDE exists (else AddInfo), config edge from Task 1 temp ini; assert each typed edge, waves place interface/base before implementer/child, a cast cycle lands in `cycles`, unknown edge kind refused, out_path refusal.
- [ ] Implement; `Cortex.Reflect+` GREEN; commit `feat(reflect): typed dependency graph export with wave order (#120)`.

### Task 4: reflect.find_name_lookups (D6)
Files: new `Operations/CortexReflectNameLookupOps.h/.cpp`, handler; test `Private/Tests/CortexReflectNameLookupTest.cpp`.
- [ ] Tests: test writes `Source/CortexSandbox/CortexTest120NameLookups.txt`? No — scanner scans `.h/.cpp` only: write a temp `Source/CortexSandbox/Private/CortexTest120NameLookups.cpp` (never compiled: created and deleted inside the test run) with `FindFunction(TEXT("DoThing"))`, `SetTimerByFunctionName`-style `FTimerDelegate::CreateUFunction(this, FName("Other"))`, `FindPropertyByName(TEXT("Health"))`; a temp BP with function `DoThing`, variable `Health` and a K2_SetTimer node with FunctionName literal `Other` plus custom event `Other`; assert cpp hits with file/line/api/name and matches, blueprint hit, plugin source excluded, `symbols` filter.
- [ ] Implement; GREEN; commit `feat(reflect): detect by-name Blueprint member access (#120)`.

### Task 5: reflect.find_duplicate_graphs (D7)
Files: new `Operations/CortexReflectDuplicateGraphOps.h/.cpp`, handler; test `Private/Tests/CortexReflectDuplicateGraphTest.cpp`.
- [ ] Tests: two BPs with identical function bodies built separately (different GUIDs/positions) → exact group; third with one extra node in a same-named function → similar group above threshold; below `min_nodes` excluded; widget trees with same class/name multiset → widget_tree similarity; blueprint_pairs summary.
- [ ] Implement; GREEN; commit `feat(reflect): detect duplicated graphs across Blueprints (#120)`.

### Task 6: Python pagination, policies, contract sync, docs (D4, D5)
Files: `MCP/src/cortex_mcp/pagination.py`, `tools/routers.py`, `response.py` (complete-or-refuse for new reflect reads), tests `test_pagination_contract.py`, `test_routers.py`; fixture + fallback; plugin `docs/` verification later by controller.
- [ ] Tests: batch_query `limit` then cursor pages return every step result exactly once with one native send; oversized batch without limit → RESPONSE_TOO_LARGE refusal (no silent drop); two identical limited requests keep independent cursors; `snapshot_id` in `_pagination` and cursor; expired cursor error unchanged; TTL 600 / 16 entries.
- [ ] Implement; Python suite GREEN; fixture mirrors new declarations; `sync_fallback --check` clean; commit `feat(mcp): batch_query pagination, cursor snapshots and #120 contract sync (#120)`.

### Acceptance (controller)
Gated build + `Cortex.Reflect+`, `Cortex.Blueprint+`, `Cortex.Core+`; Python; live sandbox smoke of every new command; Ripper live checks (`BP_Arm` config referencer, toxicity broken paths, `/Game/Ripper` dependency graph + waves, migration export) when RipperMirror builds — otherwise recorded as pending.
