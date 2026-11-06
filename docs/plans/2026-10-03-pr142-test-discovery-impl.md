# PR #142 automation test discovery implementation

## Candidate

Branch: `maintainer/pr142-test-discovery`; target main: `df0d2015d2decb8f33f5dea6837735843768791e`.

- Merge Boshchuk's contributor revision `11776c4fc946d2c5d591bb8e56703c316854c491`, retaining authored history and all 22 `.Basic` test-name changes.
- Repair three obsolete negative `remove_graph` test calls: set `dry_run=true` for missing graph, primary EventGraph, and ConstructionScript validation. Keep error-code assertions and production precondition ordering unchanged.
- Bump `UnrealCortex.uplugin` from version 15 / 0.3.3 to 16 / 0.3.4.

## Evidence

- UE 5.8 native `FAutomationReport::GetEnabledTestNames` only emits enabled leaves (`AutomationReport.cpp:129-148`).
- Source inventory finds all submitted `.Basic` names as leaves. Two additional current-main prefix collisions remain outside submitted scope.
- Native Windows/MSVC editor build succeeded before and after the test repair. The first build emitted existing UE5.8 deprecation warnings in unchanged Core/Data code; it is not a zero-warning build. The incremental repaired-test build emitted no warnings.
- RED: `Saved/TestLogs/AutomationTest_2026-10-03_210658.log`; `Cortex.Blueprint.RemoveGraph.Basic` executes and fails its three obsolete assertions with `STALE_PRECONDITION`.
- Final native execution and independent review evidence are recorded in the PR verification report after acceptance.

## Synchronization

No production C++, command, MCP, or toolkit interfaces change. Toolkit synchronization and MCP-domain benchmarks are intentionally unchanged for this test-only contribution.

## Acceptance-driven fixture repair plan

The first all-22 rendering run (`Saved/PR142NativeSmoke.log`) passed every body but exposed AssetRegistry warnings in the newly enabled Level rename/delete and Material delete tests. The full rendering suite separately crashed in GC after 144 completed tests; causation is unresolved.

1. **Level fixture owner:** update only `CortexLevelLifecycleTest.cpp`'s newly exposed rename/delete fixtures. Reuse its existing latent lifecycle pattern: create, yield one engine frame, rename/delete, yield, drain AssetRegistry gather (`WaitForCompletion`, `FlushAsyncLoading`, `Tick(-1)`), retire only fixture assets/packages, yield before releasing backing directories. Preserve existing response assertions; add real source/destination existence assertions. Use unique fixture paths and fail setup rather than passing skips after failed creation. No global GC, retries, polling, warning suppression, or production changes.
2. **Material fixture owner:** update only `CortexMaterialAssetTest.cpp`'s newly exposed delete fixture. Separate create/save from delete by an engine-frame latent boundary, drain pending registry gather before deletion, assert native deletion response and file/registry disappearance. Retire only owned fixture state and yield before directory cleanup. Do not alter production asset lifecycle behavior.
3. **Controller acceptance:** compile once after both edits; exercise the 22 exact names with rendering and check raw warnings/errors, then the full rendering suite. Root-cause the full-run GC failure from native evidence, not from the identity of the currently running test. Independently review changed fixtures before integration.

The recorded pre-fix warning run is the RED evidence for these test-lifecycle corrections. Workers must not run overlapping editor tests or builds; the controller owns shared runtime verification.

## Fixture repeatability correction

The rendering Blueprint-only isolation run (`Saved/PR142BlueprintIsolation.log`) completed 242 tests, with 240 successes and two setup failures: `AddVariable.InheritedPropertyCollision` (unchanged sibling) and newly enabled `RemoveGraph.Basic`. The repeated native smoke leaves saved fixed-name packages because original Basic cleanup marked only packages garbage. Blueprint creation always persists its package.

The two newly enabled Basic fixtures (`CortexBPAddVariableTest.cpp`, `CortexBPRemoveGraphTest.cpp`) now create GUID-unique owned paths, fail creation explicitly, and retire saved Blueprint/class state through the existing Blueprint asset owner's `delete` command with `force=true`. No pre-existing fixed-name asset is deleted by these corrections. This addresses fixture collisions and package-only retirement; it does not establish causation for the earlier broad GC crash.

Material fixture directory cleanup is non-recursive: deleting only an empty directory preserves unrelated contents if an incidental suffix collision occurs. Independent review identified and verified this correction.
