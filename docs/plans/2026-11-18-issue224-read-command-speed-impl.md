# Issue 224 Implementation Plan — Read-command speed

> **For agentic workers:** execute inline (single context owns all edits; the editor build and suite runs are shared). Steps use checkbox (`- [ ]`) syntax. Spec: `2026-11-18-issue224-read-command-speed-design.md` (decisions D1–D8).

**Goal:** eliminate avoidable synchronous loads, per-item ini/refresh/GC loops and redundant round trips on read/registration commands named in issue #224.

**Architecture:** registry-data-first enumeration (material instances, batch dirty baseline), opt-in `include_row_count` (data catalog), batched ini write + single tag-tree refresh, `SkipGarbageCollection` + one GC for dependent compiles with honest failure, load-after-dirty-check for `save_asset`, two Python caching/liveness fixes.

**Tech Stack:** UE 5.8 C++ (CortexCore/CortexData/CortexBlueprint/CortexMaterial), pytest.

## Global Constraints

- Zero-warning automation runs; `rtk proxy` for all shell output; build via the AGENTS.md UBT one-liner (not `rtk bash -lc`, which is WSL).
- Commits via explicit-path `rtk git add` + `rtk git commit` (autocrlf makes `commit.draft()` rewrite whole files).
- Never name a new test with an existing test's full name as prefix (leaf-shadowing).
- Module log categories only; `FScopedTransaction` for writes stays.
- New tags in tests: `UGameplayTagsManager::Get().AddNativeGameplayTag` at test time (never config-file or `UE_DEFINE_GAMEPLAY_TAG_STATIC`).
- Fixture suffixes from `FGuid::NewGuid().ToString(EGuidFormats::Digits).Right(8)` (never `.Left`).

## Task 1 — RED tests (all slices, one build)

**Files:**
- Modify: `Source/CortexMaterial/Private/Tests/CortexMaterialAssetTest.cpp` (or new sibling file in same dir)
- Modify: `Source/CortexData/Private/Tests/CortexListDatatablesTest.cpp`, new `Source/CortexData/Private/Tests/CortexDataTagBatchTest.cpp`
- Modify: `Source/CortexBlueprint/Private/Tests/CortexBPRecompileDependentsTest.cpp`
- Modify: `Source/CortexCore/Private/Tests/CortexAssetOpsTest.cpp`

**Test names (shadow-safe):**

1. `Cortex.Material.GetMaterialInstanceCountNoLoad.Basic` — create `UMaterial` + child `UMaterialInstanceConstant` (NewObject in temp package, `SavePackage` both with `SAVE_NoError` per repo convention), `AssetDeleted` + `MarkAsGarbage` + `CollectGarbage`; assert MIC unloaded (`FindObject<UmaterialInstanceConstant>` null); call `FCortexMaterialAssetOps::GetMaterial`; assert `instance_count == 1` and MIC still unloaded. RED today: `GetAsset()` loads every MIC → unloaded assertion fails.
2. `Cortex.Material.ListInstancesParentNoLoad.Basic` — same fixture; `ListInstances({parent_material})` → count 1, MIC unloaded.
3. `Cortex.Data.ListDatatablesNoLoad.Default` — saved 1-row DataTable fixture, GC'd; `ListDatatables({})` → entry has no `row_count` field, table unloaded. RED: field present + loaded.
4. `Cortex.Data.ListDatatablesNoLoad.IncludeRowCount` — same fixture; `ListDatatables({include_row_count:true})` → `row_count == 1`.
5. `Cortex.Data.DataCatalogNoLoad.Basic` — DataTable + StringTable saved fixtures, GC'd; `GetDataCatalog({sections:[datatables,string_tables]})` → no `row_count`/`entry_count`, assets unloaded.
6. `Cortex.Data.RegisterGameplayTagsBatch.Basic` — write 3 new tags to one temp ini via `RegisterGameplayTags`; assert `registered == 3`, file has 3 `+GameplayTagList` lines; duplicate tag inside the same batch → `already_existed == 1` and only one line. Tag roots must not collide with existing native tags (use unique `Cortex224.Test.*`-style names). RED today: intra-batch duplicate is reported via runtime-tree refresh; the file/counts assertions pass but the no-refresh-per-tag property is not directly observable — RED anchor: `already_existed` for the intra-batch duplicate **fails today**? — verify at RED run; if it passes today, the batching itself is unobservable and the test is the functional contract (drop the RED claim for this slice and note it).
7. `Cortex.Blueprint.RecompileDependentsRegistry.Basic` — parent+child BP, both compiled and `SavePackage`d, `AssetDeleted`+garbage+`CollectGarbage` (child must be genuinely unloaded); `RecompileDependents({asset_path: parent})` → `dependent_count == 1`, child up-to-date. RED: dependent_count == 0 (loaded-only discovery).
8. `Cortex.Blueprint.RecompileDependentsFailure.Honest` — parent + child; use the new `GRecompileDependentsPostCompileTestHook` (added in Task 2, guarded `#if WITH_DEV_AUTOMATION_TESTS`) to force one dependent's `Status = BS_Error` after compile; assert command fails with `CortexErrorCodes::CompileFailed` and Details contain per-item results. RED: command returns success today.
9. `Cortex.Core.SaveAssetDryRunNoLoad.Basic` — saved clean asset package, GC'd; `SaveAsset({asset_path, dry_run:true})` → `can_save == false`, `was_dirty == false`, package stays unloaded. RED: today `LoadAssetWithFallbacks` loads it.

- [x] Write tests 1-9
- [x] Build (AGENTS.md UBT one-liner, `rtk proxy`)
- [x] Run `Cortex.Material+`, `Cortex.Data+`, `Cortex.Blueprint+`, `Cortex.Core+` via `cli/Testing/RunTests.ps1`; save each log to `Saved/Issue224/RED/` immediately (RunTests rotates logs)
- [x] Record which assertions failed (expected) vs crashed/errored unexpectedly (all 8 failed at the predicted assertions; no crashes)

- [x] D1: file-local `GetHardReferencerPackages(const FName PackageName)` (asset registry `GetReferencers`, hard package deps); use in `GetMaterial` count and `ListInstances` parent filter
- [x] D2: `include_row_count` in `ListDatatables` + `GetDataCatalog`; `is_composite` via `AssetClassPath`; schema `.Optional(...)` entries in `DataCommandInfos()`
- [x] D3: two-phase `RegisterGameplayTags` per design
- [x] D4: `SkipGarbageCollection` + single GC + registry referencer union + honest failure + `GRecompileDependentsPostCompileTestHook`; `rename_scs_component` dependent loop same options
- [x] D5: `ScanPathsSynchronous(..., false)`
- [x] D6: save_asset reorder per design
- [x] D7: `if (bRollbackOnError) { FCortexBatchScope::CaptureDirtyBaseline(); }`
- [x] Rebuild; rerun the four suites; save logs to `Saved/Issue224/GREEN/`
- [x] Zero warnings/errors in suite logs (Data 156/156, Material 92/92, Blueprint 404/404, Core 241/241; tag-test notes declared via `AddExpectedMessagePlain`)

## Task 3 — Python implementation

**Files:** `MCP/src/cortex_mcp/tcp_client.py`, `MCP/src/cortex_mcp/schema_generator.py`, `MCP/tests/test_cache.py` (+ connect test file).

- [x] Add `("core", "get_operation_schema")` to `CACHED_READ_COMMANDS`
- [x] Replace connect()'s `get_capabilities` liveness call with `ping`
- [x] `schema_generator.py:851` → `{"include_row_count": True}`
- [x] pytest RED→GREEN for both; `_fallback_generated.py` regenerated offline via `scripts/sync_fallback.py --from-fixture` after adding the two params to `tests/fixtures/capabilities_cache_full.json`
- [x] Full non-e2e pytest: 1269 passed, 0 failed (`uv run --no-sync pytest tests/ -m "not e2e and not scenario and not stress"`)

## Task 4 — Gates, docs, release

- [ ] `rtk proxy python cli/clang/clang-syntax-check.py` (changed files vs origin/main)
- [x] Verification report `docs/verification/2026-11-18-issue224.md` (plugin repo) with RED/GREEN log paths
- [x] Tech-debt note for deferred item 13 (design doc carries the deferral decision; verification report and issue comment repeat it)
- [ ] Bump `UnrealCortex.uplugin` `VersionName` 0.4.18 → 0.4.19 in a dedicated `chore(release)` commit
- [x] Independent review over the diff (reviewer agent had no model; controller review found and fixed the empty-referencer `instance_count` bug; tradeoffs documented in the verification report)
- [ ] Push branch, PR, issue #224 comment; user approves merge

## Review Focus (failure modes the tests do not cover)

1. MIC nested instances (`MIC→MIC→Material`) — referencer set returns only direct children; old code also only matched direct `Parent`. Assert count semantics unchanged by review, not test.
2. Unsaved in-memory assets in `list_datatables`/catalog — registry has them via `AssetCreated`; `include_row_count` path unchanged.
3. Tag registration with multiple distinct ini files in one batch (prefix map) — each ini written once; covered by unit review since settings-based routing needs project config.
4. `save_asset` with `force=true` on an unloaded package — must still load and save.
5. Rollback-enabled batches (`rollback_on_error=true`) must still capture the baseline — covered by existing rollback tests in `Cortex.Core+`.
