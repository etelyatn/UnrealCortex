# Issue 224 — Read-command speed: no synchronous loads on enumeration paths

Design doc for [UnrealCortex issue #224](https://github.com/etelyatn/UnrealCortex/issues/224) (reported against 0.4.6 `f72a0ad`, validated against 0.4.18 `6a235d1f`).

## Validation summary at 0.4.18

| # | Item | Status |
|---|---|---|
| 1 | `material.get_material` loads every `UMaterialInstanceConstant` via `FAssetData::GetAsset()` for `instance_count` (`CortexMaterialAssetOps.cpp:494-514`) | Present |
| 2 | `material.list_instances` `parent_material` filter loads each instance (`:701-707`) | Present |
| 3 | `data.list_datatables` loads every DataTable for `row_count` (`CortexDataTableOps.cpp:237`) | Present |
| 4 | `data.get_data_catalog` same for DataTables and StringTables (`:1371`, `:1558`) | Present |
| 5 | `data.register_gameplay_tags`: per-tag ini read+rewrite and per-tag `EditorRefreshGameplayTagTree` (`CortexGameplayTagOps.cpp:247-284`, `:440`) | Present |
| 6 | `blueprint.recompile_dependents`/`rename_scs_component`: per-item `CompileBlueprint` with default GC options, no batched GC; returns success when every dependent fails; `GetDependentBlueprints` sees only loaded Blueprints (`CortexBPCleanupOps.cpp:1347`, `:1615`) | Present |
| 7 | `blueprint.list` `ScanPathsSynchronous(Paths, /*bForceRescan=*/true)` (`CortexBPAssetOps.cpp:1158`) | Present |
| 8 | `core.save_asset` loads the asset (incl. under `dry_run`) before the dirty check (`CortexAssetOps.cpp:405`) | Present |
| 9 | `core.batch`/`batch_query` capture the dirty baseline unconditionally (`CortexCommandRouter.cpp:919`) | Present |
| 10 | `core.delete_folder` per-asset `ForceDeleteObjects` | **Already fixed** — single `FCortexEditorUtils::DeleteLoadedAssets` call with one `ForceDeleteObjects` (`CortexAssetDeletionOps.cpp:235`, `CortexEditorUtils.cpp:303-341`) |
| 11 | `core.get_operation_schema` absent from `CACHED_READ_COMMANDS` (`MCP/src/cortex_mcp/tcp_client.py:32-38`) | Present |
| 12 | `connect()` sends full `get_capabilities` and discards the result (`tcp_client.py:390`) | Present |
| 13 | Composites run 4-6 sequential post-batch round trips (`blueprint/composites.py:541-616`, `material/composites.py:706-788`) | Present — **deferred by maintainer decision**; the compile-failure fallback save and informational readback have semantics that a batch step cannot express without a dedicated contract. Tracked as deferred, not silently dropped. |

Engine fact-check: `UMaterialInstanceConstant::GetAssetRegistryTags` exports no `Parent` tag (verified in UE 5.8 engine source), so the issue's "registry Parent tag" suggestion is not available; package referencer data is.

## Decisions

### D1 — Material instance enumeration (items 1, 2)

New file-local helper in `CortexMaterialAssetOps.cpp`: collect the set of package names that **hard-depend** on the material's package via `IAssetRegistry::GetReferencers` (hard package dependencies only — a MIC's only hard reference to a material is its `Parent`), then count/filter instances by `FAssetData` package membership + `UMaterialInstanceConstant` class path. No synchronous loads.

- `get_material`: `instance_count` = number of MIC-class `FAssetData` in referencer packages. Response field unchanged (always present).
- `list_instances` with `parent_material`: filter `AssetDataList` by referencer-package membership instead of loading each MIC. Without `parent_material`, behavior unchanged (already load-free).

Semantics note: a MIC referencing a material only through its `Parent` produces exactly one hard package dependency; the result matches the old `Instance->Parent == Material` check for saved assets. In-memory unsaved MICs behave as before (they were already invisible to `GetAssets` until registered).

### D2 — Data catalog row counts (items 3, 4)

New optional boolean `include_row_count` (default **false**) on `data.list_datatables` and `data.get_data_catalog`.

- Default (false): no synchronous loads. `row_count`, `parent_tables`, DataTable `top_fields` and StringTable `entry_count` are omitted. `is_composite` stays in the response, derived from `AssetData.AssetClassPath` (`UCompositeDataTable::StaticClass()->GetClassPathName()`) instead of the loaded cast.
- true: current behavior (loads each table).
- `MCP/src/cortex_mcp/schema_generator.py:851` passes `include_row_count=True` — it is the one consumer that needs row counts, and it runs on explicit request only.

### D3 — Batch tag registration (item 5)

`RegisterGameplayTags` becomes two-phase:

1. Validate every tag, resolve its ini file, check runtime-tree existence (unchanged `already_existed` semantics), dedupe within the batch.
2. Group new tags by ini file: read each ini **once**, append all its pending lines, write **once**. A tag whose line is already in the file (or pending within this batch) is reported `already_existed`, matching today's post-first-refresh behavior for intra-batch duplicates.
3. One `EditorRefreshGameplayTagTree()` after all writes, then per-tag `RequestGameplayTag` verification and result building (per-tag JSON shape unchanged).

`register_gameplay_tag` (singular) keeps its current single-tag path.

### D4 — Blueprint dependent recompilation (item 6)

`recompile_dependents`:

- Compile each dependent with `EBlueprintCompileOptions::SkipSave | SkipGarbageCollection`; one `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS)` after the loop (same pattern as `CortexBPCompileAllOps.cpp:139-144`).
- Dependent discovery: keep `FBlueprintEditorUtils::GetDependentBlueprints` (loaded), then union with asset-registry referencers of the target package filtered to `UBlueprint` class, loading each unloaded candidate and keeping only true class children (`DependentBP->ParentClass->IsChildOf(TargetBP->GeneratedClass)`). Fixes `dependent_count == 0` after an editor restart.
- Honest failure: if any dependent did not compile to UpToDate, the command returns `CortexErrorCodes::CompileFailed` with the per-item `results` array and both counts in Details, instead of success.

`rename_scs_component`: dependent compiles get `SkipGarbageCollection` with one GC after the loop (the target BP compile keeps its normal options; its failure path restores the SCS name and cancels the transaction before any deferred GC matters).

### D5 — blueprint.list scan (item 7)

`ScanPathsSynchronous(PathsToScan, /*bForceRescan=*/false)`. Registry scans still happen for not-yet-scanned paths; saved assets are already registered, so behavior is unchanged for the saved-asset case the command documents.

### D6 — save_asset load ordering (item 8)

Check `FindPackage` before loading: `was_dirty` comes from the loaded package only (an unloaded package cannot be dirty). The asset is loaded only when a save will actually happen (`bForce || bWasDirty`). Under `dry_run`, no loading at all: an unloaded package reports `can_save = bForce` (force would load+save), a loaded one reports `bForce || was_dirty` as today.

### D7 — Batch dirty baseline (item 9)

`CaptureDirtyBaseline()` runs only when `rollback_on_error` is set. The baseline is consumed exclusively by rollback verification (`PackagesDirtyBeforeBatch` is read only in `ExecuteRollback` paths, verified in `CortexCommandRouter.cpp:283`).

### D8 — Python side (items 11, 12)

- `CACHED_READ_COMMANDS` gains `("core", "get_operation_schema")`; write invalidation is derived automatically from this set, so schema responses are dropped on the next write like every other cached read.
- `connect()`: `_validate_project()` already sends `get_status`; the follow-up full `get_capabilities` round trip is replaced with `ping` (liveness only).

## Non-goals

- Item 13 (composite post-batch round trips) — deferred with maintainer approval; needs a dedicated batch-step contract to preserve compile-failure fallback save semantics.
- No new caching layer for `list_datatables` (the Python router already caches `data.get_data_catalog`).
- `core.delete_folder` — already fixed, no change.

## Verification contract

| Slice | RED→GREEN proof |
|---|---|
| Material (D1) | New automation tests: saved material+MIC, garbage-collected, then `get_material` reports `instance_count==1` **and** the MIC stays unloaded; `list_instances(parent_material)` returns it without loading. |
| Data catalog (D2) | New tests: default `list_datatables` omits `row_count` and leaves a saved table unloaded; `include_row_count=true` reports the correct count. Same for `get_data_catalog` (DataTable + StringTable sections). |
| Tags (D3) | New test: batch of 3 tags into one ini — all registered, one file write with 3 lines, intra-batch duplicate reported `already_existed`, `registered/already_existed/failed` totals correct. |
| Blueprint (D4) | New tests: saved+unloaded child BP discovered via registry and recompiled; honest-failure via existing `WITH_DEV_AUTOMATION_TESTS` post-compile test-hook pattern (test forces a dependent's status to compile-failed, command must return `COMPILE_FAILED`). Existing `RecompileDependents.*` tests stay green. |
| Core (D6) | New test: saved clean package, GC'd, `save_asset dry_run` returns `can_save=false` and leaves the package unloaded. |
| Core (D7), Blueprint (D5) | No observable behavior change; gated by existing suites (`Cortex.Core+`, `Cortex.Blueprint+`) and review. |
| Python (D8) | pytest: second `core.get_operation_schema` router call is a cache hit (no TCP round trip); connect handshake sends `ping`. |

Test-name safety: existing `Cortex.Data.ListDatatables` and `Cortex.Blueprint.RecompileDependents.*` must never be used as a path prefix (UE only executes leaf report nodes).
