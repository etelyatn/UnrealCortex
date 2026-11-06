# Issue 180 — Token efficiency, Task 4: Data asset selectors + truthful search projection/pages

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `89611e195007e999febf9bd061de26ec01bc7988` (Task 3 completion)
**Owner:** Task 4 implementer (`NativeAssetInventory`) — TDD staging, implementation, docs and smoke
script; all builds, test runs, smokes and design rulings were controller-owned.

## Scope

Native `CortexData` discovery reads now honour their advertised selectors and report truthful
paging, and the affected consumer test was migrated off the removed scalar selectors.

Changed files (this commit):

| File | Change |
|---|---|
| `Source/CortexData/Private/Operations/CortexAssetSearchOps.h` | Declares the internal `ComputePageEnd` helper beside `SearchAssets`. |
| `Source/CortexData/Private/Operations/CortexAssetSearchOps.cpp` | `search_assets` implemented to the TE04 contract (arrays, exact class names, canonical prefixes, validation, projection, `count_only`, native `offset`/`limit` paging); overflow-safe `ComputePageEnd`; case-sensitive canonical ordering. |
| `Source/CortexData/Private/Operations/CortexDataAssetOps.cpp` | `ListDataAssets` reads the advertised `class_name` (subclass-aware); `class_filter` no longer read there. |
| `Source/CortexData/Private/CortexDataCommandHandler.cpp` | `search_assets` operation metadata advertises `fields`, `count_only`, `offset` and tightened array descriptions. |
| `Source/CortexData/Private/Tests/CortexDataAssetSearchTest.cpp` | Consumer-visible regressions: folder/class/query filters, unions, deterministic paging, `count_only`, projection, malformed-array refusals, case-order and window-boundary cases; disposable GUID fixtures with latent, frame-separated retirement. |
| `Source/CortexData/Private/Tests/CortexDataAssetCrudTest.cpp` | `list_data_assets` `class_name` selector regression and removed-alias regression with a disposable list fixture. |
| `Source/CortexData/Private/Tests/CortexDataAssetSubclassFilterTest.cpp` | Migrated the existing `list_data_assets` requests from `class_filter` to `class_name` (clean cutover of a registered native caller). |
| `MCP/tests/test_e2e.py` | `TestAssetSearch::test_search_assets` migrated from the removed `path_filter` scalar to the array/`fields`/`count_only` contract with exact, population-derived assertions. |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task4.md` | This record. |

Not committed here (deliberately): the parent-repo `docs/systems/cortex-data.md` update (kept local
for Task 17), the task-owned smoke script under `.superpowers/`, and any toolkit change.

## Contract implemented

`data.search_assets`

* Params: `query` (case-insensitive name-or-path text), `class_names:string[]` (OR within, exact
  class-name identity — no subclass expansion), `path_prefixes:string[]` (OR within; canonical
  mounted package folders matched recursively on component boundaries), `fields:string[]` (whitelist
  `path`, `name`, `class_name`, `package_path`; default `[path, name, class_name]`), `count_only:bool`,
  `offset:number`, `limit:number` (default 50).
* Malformed arrays refuse with `INVALID_FIELD` before any registry fetch: non-array values, non-string
  elements, empty class names, non-canonical prefixes, and non-whitelisted projection fields.
  `FJsonValue::TryGetString` is a *conversion*, so a strict `Value->Type == EJson::String` check is
  required for every selector array.
* Filtering runs over every candidate before paging, so `total_matches` is exact and independent of
  `limit`/`offset`; matches are ordered by canonical object path (`(PackageName, AssetName)`
  case-sensitive comparison) before slicing; `count_only` skips ordering and row construction.
* Response: `assets`, `count` (returned rows), `total_matches`, applied `offset`/`limit`,
  `next_offset` (= `offset + returned`) and `is_complete`.
* `total_before_limit` and the scalar `class_filter`/`path_filter` reads were removed.

`data.list_data_assets`

* Uses the advertised `class_name` (subclass-aware resolution and `path_filter` unchanged); the
  obsolete `class_filter` name is no longer read for this command. Raw-export compatibility names in
  `CortexDataExportOps.cpp` are untouched.

## Controller-observed RED → GREEN evidence

All runs were serial, controller-owned, on the owned editor of the time, with a raw
`Warning|Ensure|Fatal|Assertion` scan of each log; every listed log is
`Saved/TestLogs/AutomationTest_2026-10-13_<stamp>.log` as reported by the controller.

**Staged RED (before implementation)**

* Compile repair round 1: native RED build failed `exit 6` on unity redefinitions of the staged
  anonymous-namespace helpers `CreateDataRouter`/`CleanupLoadedPackage`/`DeletePackageFile` shared
  with existing export/load-guard fixtures, plus a new C4996 from the deprecated bool
  `GetObjectsWithPackage`. Repaired by renaming the staged helpers (Search/List prefixes) and using
  `EGetObjectsFlags::None` on 5.8+ behind `UE_VERSION_OLDER_THAN(5,8,0)` for the pre-5.8 floor;
  rebuild `exit 0`, 4 actions, 26.28 s (`task-4-red-build2.log`), only the pre-existing 3 C4996
  warnings outside the new fixtures.
* `Cortex.Data.SearchAssets+`: 9 tests, 9 FAIL, 37.71 s — absent `total_matches`/`offset`/
  `next_offset`/`is_complete`, ignored `class_names`/`path_prefixes`/`fields`/`count_only`,
  malformed input accepted (`214556.log`).
* `Cortex.Data.DataAsset+`: 11 tests, 8 PASS / 3 FAIL, 37.79 s — `ListClassFilterRemoved` (0 vs 2),
  `ListClassNameSelector` (2 vs 1, missing `resolved_class`) and the migrated
  `DataAssetSubclassFilter` (0 vs broad) (`214647.log`).

**Implementation GREEN rounds**

* Round 1: `Cortex.Data.SearchAssets+` 9 tests, 8 PASS / 1 FAIL, 65.21 s — only the numeric
  array element was accepted (`220415.log`); the strict `EJson::String` type check was then added.
  `Cortex.Data.DataAsset+` 11/11 GREEN, 34.31 s (`220645.log`).
* RED round 3 (window/case staging, compiled strict-guard + case-insensitive ordering source):
  `Cortex.Data.SearchAssets.WindowBoundaryMath` 1/1 FAIL, 68.23 s, expected 2, got `-2147483648`
  (`224845.log`); `Cortex.Data.SearchAssets.CaseSensitiveCanonicalOrder` 1/1 FAIL, 41.11 s, order
  `alpha, Beta` (`225354.log`). The controller then authorized the arithmetic and comparator fixes
  and ruled that the full-consumer `WindowLargestLimit` test must not run before them.

**Final GREEN (both fixes applied, zero warnings in every raw scan)**

| Filter | Result | Log |
|---|---|---|
| `Cortex.Data.SearchAssets.WindowBoundaryMath` | 1/1 GREEN | `AutomationTest_2026-10-13_230224.log` |
| `Cortex.Data.SearchAssets.WindowLargestLimit` | 1/1 GREEN | `AutomationTest_2026-10-13_230301.log` |
| `Cortex.Data.SearchAssets.CaseSensitiveCanonicalOrder` | 1/1 GREEN | `AutomationTest_2026-10-13_230337.log` |
| `Cortex.Data.SearchAssets+` (full) | 12/12 GREEN | `AutomationTest_2026-10-13_230414.log` |
| `Cortex.Data.DataAsset+` (full) | 11/11 GREEN | `AutomationTest_2026-10-13_230452.log` |

## Registered smoke and live e2e

* `smoke-task4-search-assets.py` (task-owned, retained until review) through the shipped stdio server
  and the real registered `data_cmd`/`editor_cmd`: `TASK4_SMOKE_OK` in 5.44 s on owned editor
  PID 58376 / port 8743 — exact `total_matches == 2` with `limit=1`, two deterministic pages
  (Alpha then Beta), `class_names`/`path_prefixes` OR unions, `count_only` with no rows, both
  projections (`path` and folder-valued `package_path`), 9 malformed-input refusals, and clean
  registry + on-disk retirement.
* Migrated live e2e `tests/test_e2e.py::TestAssetSearch` PASSED in 0.31 s on owned editor PID 18272.
  The controller changed only the smoke script's decoder for the `run_python` per-argument record
  shape; no product change was involved.
* All owned editors were closed normally with actual-PID proof.

## Source-confirmed rulings

* **Fixture lifecycle (deleted package "modified on disk").** `AssetRegistry.cpp:8475-8505` logs when
  a queued `FCA_Added`/`FCA_Modified` is processed for a package already in `CachedEmptyPackages`.
  Saving a package and then deleting it in the same engine frame leaves that queued event behind, so
  the fixtures now create/save in the test frame, yield one full engine frame via a latent command,
  and retire in a later frame after `WaitForCompletion() + FlushAsyncLoading() + AssetRegistry.Tick(-1.0f)`.
* **Window arithmetic overflow.** `Offset + Limit` with `offset=1, limit=INT32_MAX` wrapped to
  `INT32_MIN`; `FMath::Min(Total, …)` then returned `INT32_MIN`, and the unchecked
  `PageEnd - Offset` wrapped to `INT32_MAX`, so the unfixed path could attempt a ~32 GiB reserve.
  The extracted helper now widens to `int64` before clamping, which is identical to the previous
  expression for every non-wrapping input.
* **Case semantics.** `FName::LexicalLess` → `FName::Compare` → `FNameEntryId::CompareLexical` is
  documented "ignores case" (`NameTypes.h:83-85`), which orders case-variant names non-canonically;
  `FName::CompareSensitive` ("case-sensitive and alphabetical ascending", `NameTypes.h:946-951`) gives
  canonical object-path order.

## Ownership delta

`Source/CortexData/Private/Operations/CortexAssetSearchOps.h` now owns one additional internal
member: `static int32 FCortexDataAssetSearchOps::ComputePageEnd(int32 TotalMatches, int32 Offset,
int32 Limit)`, defined beside `SearchAssets` in the same `.cpp` and called only there. It is exposed
for this module's allocation-free boundary regression (`Cortex.Data.SearchAssets.WindowBoundaryMath`)
so the window arithmetic can be proven without triggering the reserve path. It is not a command,
schema or MCP surface, and it introduces no new tool or alias.

## Task 17 handoff (not done here)

* `MCP/src/tools/data/assets.py::search_assets` still sends `class_filter`/`path_filter` and must send
  `class_names`/`path_prefixes` (arrays), optionally surfacing `fields`/`count_only`/`offset`.
* `MCP/tests/fixtures/mcp_tool_signature_snapshot.json` and the generated fallback/routers still list
  the old selectors for the `data.search_assets` tool signature.
* Toolkit/MCP `list_data_assets` callers still using `class_filter` must move to `class_name`.
* Consumer-visible response deltas: `total_before_limit` is gone (use `total_matches`); new
  `offset`/`limit`/`next_offset`/`is_complete`; rows are projected by `fields`.
* `docs/systems/cortex-data.md` (parent repo) documents the new contract locally and is intentionally
  not part of this commit.
