# Project-wide dependency map: config references, batch analysis export, typed dependency graph

Date: 2026-11-05
Status: Delivery authorized by user on 2026-11-04 (merged delivery; executed after #119 lands)
Issue: https://github.com/etelyatn/CortexSandbox/issues/120
Reporter: etelyatn (Ripper Blueprint→C++ migration audit, 2026-11-04)
Implementation owner: UnrealCortex / CortexReflect, CortexBlueprint, MCP Python facade
Depends on: #119 merged as UnrealCortex b2b3ea2c (shared type vocabulary `FCortexBPAssetOps::GetBlueprintTypeNames`, `graph_metrics`, analyze `sections` via `FCortexBPAnalysisOps::GetMigrationSectionNames`). Base: b2b3ea2c, branch `feat/issue120-dependency-map`.
Delivery endpoint: merged UnrealCortex PR; toolkit reference sync in a cortex-toolkit PR; parent gitlinks excluded.

## Intent and acceptance

Migration-wave planning must be possible from a few calls: who references an asset (including config), what is broken in config, a typed project dependency graph with a wave order, and a one-call analysis export.

| Criterion | Current evidence | Route | Minimal proof and exit |
|---|---|---|---|
| `get_referencers` includes Config/INI references (`type:"config"`) | Asset Registry only (CortexReflectOps.cpp:2014-2103); no INI reader in plugin | Implement | Native test with a temp ini under a scanned config dir referencing a sandbox asset; Ripper `BP_Arm` live check |
| `reflect.find_broken_config_paths` reports missing package/directory paths | none | Implement | Native test: temp ini with an existing and a missing path; Ripper toxicity penalty paths live |
| `blueprint.export_migration_analysis` writes JSONL + index in one call | single-asset analyze only | Implement | Native test over a temp folder: N rows, index fields, sections honoured, path policy refusals; Ripper live one-call run |
| `reflect.export_dependency_graph` writes typed edges + wave order | per-asset/per-symbol reads only | Implement | Native test fixture covering every edge kind available in sandbox; waves respect inherits/implements; Ripper `/Game/Ripper` live |
| `core.batch_query` pagination | Python generic truncation silently drops step results (response.py) | Implement (Python) | pytest: limit/cursor pages complete results; batch executed once; oversized without limit refuses |
| Detect by-name access to Blueprint members from C++ and BP timers | no source scan; find_usages ignores string pins | Implement | Native test: temp source-like file in a scanned dir is not allowed (see D6) → fixture via BP K2_SetTimer node + sandbox Source scan of a test-owned pattern |
| Detect duplicated graphs across Blueprints | fingerprints are GUID/position sensitive | Implement | Native test: two BPs with identical function bodies (different GUIDs/positions) grouped exact; near-identical grouped similar |
| Optional: longer cursor TTL / snapshot id | `PaginationCache(5, 60s)`, same-key store replaces snapshot | Implement (cheap) | pytest: two identical requests keep independent cursors; snapshot_id exposed |

## Decisions

### D1 Config scanner (shared by D1/D2/D4)
Private CortexReflect helper `CortexReflectConfigScan::Scan() -> TArray<FConfigReference>` with `{File (project-relative), Line, Section, Key, Value, Path}`. Files: `FPaths::ProjectConfigDir()/*.ini` (non-recursive, excludes `Saved/`), and `Config/*.ini` of every enabled plugin whose base dir is under `FPaths::ProjectDir()` (project plugins, not engine plugins). Raw line scan (keeps line numbers): track `[Section]`, skip `;`/`#` comments, split `Key=Value` (strip `+ - . !` prefixes). Extract every token matching a long package path whose first segment is a registered content root (`FPackageName::QueryRootContentPaths`, `/Script/` excluded): `/Root/[A-Za-z0-9_\-/.]+`, trimmed of quotes/parens/commas. Normalise to package name (`ObjectPathToPackageName`, strip trailing `_C`/subobject). Directory-like tokens (trailing `/`, or no package and no `.` in the leaf) are tagged `kind:"directory"`.

`get_referencers` appends rows `{package:<ini file>, type:"config", file, line, section, key}` for tokens whose package equals the queried package; `total`/`total_unfiltered` include them; new optional `include_config:bool=true`. `limit` counts all rows.

`reflect.find_broken_config_paths {}` → `{scanned_files, checked, broken:[{file,line,section,key,value,path,kind}]}`. A package token is broken if neither `FindPackage` nor `FPackageName::DoesPackageExist`; a directory token is broken if neither Asset Registry `PathExists` nor the mapped disk directory exists.

### D2 blueprint.export_migration_analysis
Params: `paths:array[string]` or `path_prefix:string` (exactly one), `types:array[string]` (blueprint.list vocabulary), `sections:array[string]` (analyze vocabulary plus `graph_metrics`), `out_path:string` (required; existing export convention `out_path`, not `output_path`; resolved by `FCortexSafeFileContract::ResolveWritePath`, i.e. under Saved/Content/Config). Writes JSONL (one `{asset_path, analysis, graph_metrics?}` per line, error rows `{asset_path, error_code, error_message}`) atomically, and `<out_path minus extension>.index.json` with rows `{name, asset_path, type, parent_class, native_parent_class, parent_chain:[BP ancestors], metrics:{functional_nodes, has_tick, unreachable_nodes, conditionals}, violations, migration_confidence, unsupported_node_types, blockers:[...]}`. Implementation reuses the analyze builder and graph_metrics builder in-process (no TCP calls). Read-only for assets; writes only the export files.

Enumeration and request validation mirror `blueprint.compile_all` (CortexBPCompileAllOps.cpp): exactly one of `path_prefix`/`paths`, mounted-folder validation, Asset Registry enumeration without loading, level-script Blueprints excluded, `types` validated against `FCortexBPAssetOps::GetBlueprintTypeNames()` (case-insensitive), missing paths reported as rows, assets blocked by the mutation guard skipped with a reason. Sections default to all of `GetMigrationSectionNames()` plus `graph_metrics`; unknown names refused before any load. Each Blueprint goes through the same in-process builders as `analyze_for_migration` and `graph_metrics` (shared functions, not copies); garbage is collected once at the end. Index written with `WriteJsonReportAtomic`; the JSONL with the same temp-file-then-move semantics. Response mirrors compile_all: `{out_path, index_path (absolute), totals:{total, exported, failed, not_found, skipped, excluded_by_type}, by_type, first_failures (≤10)}`.

### D3 reflect.export_dependency_graph
Params `path_prefixes:array[string]` (required), `edge_kinds:array[string]` (default all), `out_path` (required, same policy). CortexReflect gains private deps `UMG` (widget trees) only; no dependency on CortexBlueprint/CortexGraph.
Edge derivation (from → to, "from depends on to"):
- `inherits`: ParentClassPath tag (no load).
- `implements`: loaded BP `ImplementedInterfaces`.
- `calls_message`: `UK2Node_Message` → interface class of the called function.
- `casts`: `UK2Node_DynamicCast::TargetType`.
- `spawns`: `UK2Node_SpawnActorFromClass` class pin default object.
- `creates_widget`: `UK2Node_ConstructObjectFromClass` nodes whose class is `K2Node_CreateWidget` (by class name) → class to spawn.
- `widget_tree_child`: widget tree widgets whose class is a Blueprint-generated user widget.
- `listview_entry`: widgets with an `EntryWidgetClass` class property set.
- `level_placement`: World assets under prefixes whose Asset Registry hard dependencies include the BP package.
- `struct_use` / `enum_use`: Asset Registry dependencies to UserDefinedStruct / UserDefinedEnum assets.
- `config`: D1 references (from = ini file node).
Targets normalised to asset object paths (generated classes `_C` → BP asset). Nodes: every asset under the prefixes plus referenced external targets flagged `external:true`. Waves: Kahn topological order over in-prefix asset nodes using all asset→asset edges (config/ini nodes and external targets excluded); remaining strongly connected sets go to `cycles` and are appended as a final wave. File: `{nodes, edges:[{from,to,kind,graph?}], waves, cycles}`. Response: `{out_path, nodes, edges_by_kind, waves:[sizes], cycles}`.

### D4 core.batch_query pagination (Python)
`limit`/`cursor` accepted by the batch_query facade: the batch executes once, `results` are stored in the pagination cache, pages are served from cache (never re-executing). Without `limit`, an oversized response is refused (`RESPONSE_TOO_LARGE`, suggestion `limit`) instead of silently dropping step results.

### D5 Pagination snapshot/TTL (Python)
`PaginationCache(max_entries=16, ttl_seconds=600)`; each store gets a monotonically increasing `snapshot_id` included in the cursor and `_pagination`; identical requests no longer overwrite each other's snapshot (key = request hash + snapshot_id).

### D6 reflect.find_name_lookups
Params `path_prefixes:array[string]` (Blueprints to match against), `symbols:array[string]` optional filter. C++: scan `.h/.cpp` under `FPaths::GameSourceDir()` and project plugins' `Source/` (fixed roots, no caller-supplied paths) for string-literal arguments of `FindFunction`, `FindFunctionByName`, `FindFunctionChecked`, `FindPropertyByName`, `FindFProperty`, `SetTimerByFunctionName`, `K2_SetTimer`, `CreateUFunction`, `BindUFunction` (literal forms `TEXT("X")`, `"X"`, `FName("X")`, `FName(TEXT("X"))`). Blueprint: `UK2Node_CallFunction` calling `K2_SetTimer`/`K2_SetTimerForNextTick`/`SetTimerByFunctionName`-style functions with a literal `FunctionName` pin. Each hit `{source:"cpp"|"blueprint", file|asset_path, line|node_id, api, name, matches:[{asset_path, member, member_kind}]}` where matches are BP functions/events/variables/custom events with that name under the prefixes. Plugin's own source is excluded.

### D7 reflect.find_duplicate_graphs
Params `path_prefixes` (required), `min_nodes:int=5`, `similarity:number=0.9`. Per function/macro/ubergraph event-chain graph: normalised hash over canonical node descriptors (class, member reference, non-default pin values, link topology by descriptor index; excludes GUIDs, positions, comments, knots). Exact groups = identical hash. Similar groups = graphs with the same name in different Blueprints whose node-descriptor multiset Jaccard ≥ `similarity`. Widget trees compared the same way (class+name multiset) as `kind:"widget_tree"`. Response `{exact_groups, similar_groups, blueprint_pairs:[{a,b,shared_graphs,widget_tree_similarity}]}` sorted by size; complete-or-refuse in Python.

## Contracts and synchronisation
New commands declared in handlers; fixture + generated fallback; toolkit `mcp-tool-reference.md`/`reflect-patterns.md` sync. File-writing commands are not on the Blueprint read-only bypass list for mutation guards but perform no asset mutation; they never save assets.
