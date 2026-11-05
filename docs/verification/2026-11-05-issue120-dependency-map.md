# Issue 120: project-wide dependency map

## Disposition and identity

- Request: https://github.com/etelyatn/CortexSandbox/issues/120 (reporter etelyatn, Ripper Blueprint→C++ migration audit 2026-11-04).
- Route: implement. Merged UnrealCortex PR, a parent `docs/systems` PR in CortexSandbox, and a cortex-toolkit reference sync. Parent gitlinks and pre-existing edits (`MCP/pyproject.toml`, `MCP/uv.lock`, `UnrealCortex.uplugin`) are excluded.
- Plugin base: main `d8003fa1` (after the #119 hardening). Branch `feat/issue120-dependency-map`. Candidate head and final runs are recorded below.
- Design and plan: `docs/plans/2026-11-05-issue120-dependency-map-{design,impl}.md` (D1–D7; the design records the final semantics).

## What shipped

- **Config-aware referencers.** `reflect.get_referencers` returns `type:"config"` rows from project and project-plugin `Config/*.ini`. The `include_config` flag defaults to true, and config rows are kept within `limit`.
- **Broken config paths.** New `reflect.find_broken_config_paths` reports missing packages and directories. Its tokeniser keeps every valid package character, `+` included, and skips `*Redirects` old names and wildcards.
- **One-call migration export.** New `blueprint.export_migration_analysis` writes a JSONL file plus an index (parent chain, metrics, violations, blockers) in one call. It uses the same builders as `analyze_for_migration` and `graph_metrics`, and shares its enumeration helper with `compile_all`.
- **Dependency graph.** New `reflect.export_dependency_graph` writes typed edges to a file: inherits, implements, calls_message (Message and typed interface calls), casts, spawns, creates_widget, widget_tree_child, listview_entry, level_placement (World hard dependencies plus OFPA external actors), struct_use, enum_use and config.
  - Waves are computed by SCC condensation with longest-path levels. Hard kinds order the waves by default; `ordering_kinds` overrides this.
- **By-name member access.** New `reflect.find_name_lookups` covers literal by-name access to Blueprint members from C++ (FindFunction, FindPropertyByName, FindFProperty, CreateUFunction, BindUFunction, AddUFunction, CallFunctionByNameWithArguments and timer APIs) and from Blueprint timer nodes. Matches cover functions, events, custom events, variables and components.
- **Duplicate graphs.** New `reflect.find_duplicate_graphs`:
  - Exact groups use self-normalised strict descriptors.
  - Similar groups use a loose descriptor, where Blueprint class references become their native ancestor. They are star clusters with true min/max similarity.
  - Widget trees are compared by (name, native class, parent).
  - `blueprint_pairs` are reported.
  - It also takes an optional `out_path`.
- **MCP facade.**
  - `core.batch_query` gained `limit`/`cursor`. It executes once; an oversized response is served by cursor and never re-executed.
  - Pagination snapshots carry a `snapshot_id` (16 entries, 600 s).
  - The long transport window applies to the whole-project scans.
  - Complete-or-refuse policies cover the new reads.
  - The capabilities fixture and the generated fallback are synced.

## Acceptance matrix

| Criterion | Result | Evidence |
|---|---|---|
| On Ripper, `get_referencers` for `/Game/Ripper/Core/MC/Parts/BP_Arm` includes the `DefaultGame.ini` reference | PASS | Live RipperMirror: config row `Config/DefaultGame.ini:23 [/Script/Ripper.RipModularCharacterSettings] ModularCharacterConfig`. Native test `Cortex.Reflect.Config.ReferencersIncludeConfig`. |
| `find_broken_config_paths` reports the toxicity penalty paths | PASS | Live: 7 files, 368 tokens checked, 7 broken. These include the 4 `/Game/Ripper/Gameplay/Effects/ToxicityPenalties/GE_*` classes from `DefaultGame.ini:278`. Native `Cortex.Reflect.Config.BrokenPaths` and the token-shape tests. The sandbox `+` false positive was found live and fixed. |
| `export_dependency_graph` over `/Game/Ripper` writes typed edges for every BPI implementer and caller, every BP→BP inheritance chain, plus a wave order | PASS | Live, in 7.7 s: 1213 nodes. Edges: inherits 829, implements 103, calls_message 117, widget_tree_child 492, listview_entry 40, casts 197, creates_widget 20, spawns 8, struct_use 19, enum_use 7, level_placement 127, config 161. 9 waves, 0 cycles, 0 unresolved. Native `Cortex.Reflect.DependencyGraph.*`. |
| `export_migration_analysis` reproduces the audit dataset in one call | PASS | Live `/Game/Ripper`: 829 rows exported, 0 failed, 5.0 MB JSONL plus index. By type: Widget 316, Object 350, Actor 110, AnimBlueprint 21, Interface 17, EditorUtility 10, Component 4, FunctionLibrary 1. `WBP_PatientMeetPopup` row: 21 widgets and populated `references_widgets`. Native `Cortex.Blueprint.MigrationExport.*`. |
| `batch_query` pagination | PASS | pytest (limit, cursor, oversized without limit, single oversized step; one native send each). Live fresh-process smoke: 5 step results over limit-2 pages, 1 native send, `snapshot_id` present. |
| Detect C++ access to Blueprint members by name | PASS | Live 1079 files: `RipPatientDebugHelpers.cpp` FindPropertyByName/FindFunction (OverlappingPatient, AttachPatient, PatientAttachmentTransform, SetCurrentPatient); `RipSurgeryConsoleCommands.cpp:250` FindFunction RequestEndOperation; `BP_Lift` K2_SetTimer `RemoveUpgradeAppearEffect`. Native `Cortex.Reflect.NameLookup.*`. |
| Detect duplicated graphs across Blueprints | PASS | Live `WBP_Patient_Body` vs `WBP_Patient_Profile_Body`: `widget_tree_similarity` 1.0; similar groups GetAllHubsExceptType 0.942, GetHubsByType 1.0, UpdateArms 0.925. The other shared functions differ in node count (for example 7/3 and 11/30). Project-wide: 94 exact groups, 20 similar groups. Native `Cortex.Reflect.DuplicateGraphs.*` (SiblingClasses, SelfReference, Precision, SimilarClusters). |
| Optional longer cursor TTL / snapshot id | PASS | 16 entries, 600 s, `snapshot_id` in cursor and `_pagination`; pytest. |

## Runs

Runs were made at the stated revisions. Controller runs are listed separately from implementer runs.

- **Ripper live.** The user closed their editor. RipperMirror's plugin was checked out at `ea47f607` (duplicate check re-run at `ed2c4f3b`) from a local fetch. RipperEditor built with `Result: Succeeded`, and the checks ran in a task-owned editor. The checkout was then restored to `74244092` and rebuilt.
- **Sandbox live.** Task-owned editor: all new commands were exercised, and an `out_path` outside the allowed roots was refused with `INVALID_FILE_PATH`.
- **Merged-base gate.** At `0894d510`: Blueprint 313/313, Reflect 70/70, Python 1119.
- **Fix wave.** Controller `Cortex.Blueprint+` at `ea47f607` passed 314/314.
- Final candidate (code at `3b312c10`, plus this report): controller gated build `Result: Succeeded`; `Cortex.Reflect+` 88/88 (`AutomationTest_2026-11-05_141650`), `Cortex.Blueprint+` 314/314 (`141801`), `Cortex.Core+` 207/207 (`141937`); Python non-e2e 1154 passed; `sync_fallback --from-fixture --check` OK; fixture parity with native reflect/blueprint declarations verified programmatically.
- **Environment.** An implementer reported a crash ordering in `RemoveSCS` that did not reproduce (`RemoveSCS+` 15/15; full suite green). It matches the known environment flake that appears while another editor runs.

## Limitations

- Duplicate similarity compares descriptor multisets per same-named graph; renamed functions are not paired, and collapsed graphs are not expanded.
- `find_name_lookups` detects literal names only, not names passed through variables or constants.
- `level_placement` for OFPA maps is derived from each World's external actor packages and covered by a native fixture (see the follow-up section). On Ripper, 127 placements were observed (map package plus external actors combined) before that change.
- Blueprints loaded by the whole-project scans stay resident until editor GC (`RF_Standalone`), the same as `compile_all`.

## Slow-model review follow-up

Source: independent post-merge review of PR #192 (merge `1e224991`), seven P2 findings, all confirmed against the source before fixing (design: `docs/plans/2026-11-05-issue120-dependency-map-design.md`, "Slow-model review follow-up"). The Limitations entry "level_placement for OFPA maps is unit-tested through path mapping" above is superseded by the native OFPA fixture below.

| Finding | Test (RED before the fix, GREEN after) |
|---|---|
| I1 expired batch cursor advises re-sending | `pytest test_pagination_contract.py::TestBatchQueryPagination::test_expired_batch_cursor_never_advises_resending_the_batch` (RED: message was "Re-send the original command with 'limit'") |
| I2 StrictParams not enforced for Blueprint commands | `Cortex.Blueprint.StrictParams.DeclaredContractEnforced` (RED: `type`/`section` accepted, export written; all five strict commands covered; declared params still accepted) |
| I3 OFPA placement of out-of-prefix Blueprints | `Cortex.Reflect.DependencyGraph.ExternalActorOutsidePrefix` (saved OFPA map in the exported prefix, external actor of a Blueprint in another folder; RED: no edge, no node) |
| I4 widget variables as members | `Cortex.Reflect.NameLookup.WidgetMembers` (RED: `HealthText` unmatched) |
| I5 strict widget hash short class names | `Cortex.Reflect.DuplicateGraphs.StrictWidgetClassIdentity` (RED: H1, H2, H3 grouped; GREEN: H1 and H3) |
| I6 export pair after index failure | `Cortex.Blueprint.MigrationExport.PairConsistency` (index held open so replacing it fails: RED left the new JSONL beside the old index, and a new JSONL beside a stale index when none existed before) |
| I7 package folder characters | `Cortex.Reflect.Config.PackageFolderCharacters` (RED: `/Game/Folder`, `/Game/Set`, `/Game/Dir`, `/Game/Odd`, `/Game/Maps`) |

RED ran against the pre-fix production code with the new tests in place (gated build `Result: Succeeded`): all six native tests failed 6/6, the Python test failed 1/1. GREEN: `Cortex.Blueprint.StrictParams` 1/1, `MigrationExport` 7/7, `Reflect.Config` 6/6, `NameLookup` 6/6, `DuplicateGraphs` 10/10, `DependencyGraph` 6/6 (the `ExternalActorWorld` path-mapping test was removed with `ResolveExternalActorWorld`). Final gate at the follow-up head: `Cortex.Blueprint` 319/319, `Cortex.Reflect` 91/91, `Cortex.Core` 207/207, Python non-e2e 1155 passed; capabilities fixture/fallback parity `--check` OK (no declaration changed).

Notes: widget animations are indexed from the same `GeneratedVariables` source as variable widgets (`widget_animation` kind); the native test exercises the variable-widget path end to end, a bare `UWidgetAnimation` fixture was not constructed. The OFPA fixture needs the registry to scan the saved external actor package; it does so through `ScanModifiedAssetFiles`.
