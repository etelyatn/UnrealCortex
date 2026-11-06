# StateTree PR 172 Implementation Plan — local candidate verified

> **For agentic workers:** REQUIRED SUB-SKILL: Use executing-plans for native execution, or subagent-driven-development if selected after plan review.

**Goal:** Locally verify both contributor behaviors with consistent selectors, genuinely paged serialization and strict input validation; preserve their authorship and source identity. Final patch-version bump/review is controller-owned. No push/merge is authorized or claimed.

**Architecture:** Extend existing CortexST shared enumeration and dump_tree inspection owners. Keep statetree_cmd as the transport tool; do not add tool surfaces or mutation behavior. Page reference descriptors before reflecting stored values.

**Tech Stack:** Unreal Engine C++, native automation, Python MCP, UE 5.8.3 local editor.

**Spec:** [Accepted design](2026-10-07-statetree-pr172-design.md)

## Global constraints

- Plugin supports UE 5.6+; local verification is UE 5.8.3.
- UObject operations remain Game Thread only, no compile/save from inspection.
- Normal branches in Plugins/UnrealCortex, no worktrees or destructive Git operations.
- All shell commands through rtk. Serialize native test runner/editor ownership.
- Preserve contributor authorship and unrelated parent gitlinks.
- Maintainer-authored behavior requires observed RED then GREEN.
- Final patch bump: 0.3.5 → 0.3.6; descriptor revision 17 → 18, subject to refreshed version state.

**Completed source:** base `70ece02d9fa5e0145501b1f14412545780d898f7`; retained mavka-games originals `6a87b744dcbd394c297937c7f63468680a5fcaf2`, `b23fbb510659402946db98edf6ca5e736b538893`; task 1 `00297cf3bb4d5ccd3ae41b1a21e41ed047a62fcb`; frozen native/MCP acceptance source `f41162ffd733b2223d364ee6c8ced5cbc5b26294`. Status: implementation and local behavior acceptance complete; not remotely shipped. Controller applies/reviews Version 18 / VersionName 0.3.6 last, separate from behavior evidence gathered at 17 / 0.3.5.

## Review focus

1. A null first subtree must not hide later valid states.
2. Paths duplicated across roots must produce the established ambiguity error.
3. Paging malformed types must not fall back to a full-tree response.
4. Paging must not reflect off-page instance bodies or lose node owner identity.
5. Stored unsupported/fixed-array/container values must remain truthful, with diagnostics and no asset mutation.

## Task 1: Consistent multi-root selection

**Files:** CortexSTTypes.h/.cpp; Operations/CortexSTStateOps.cpp, CortexSTInspectOps.cpp; transition callers found by references; Tests/CortexStateTreeInspectTest.cpp and existing state/transition tests, all under Source/CortexStateTree/Private.

**Interface:** `CortexST::CollectAllStates(const FCortexSTAssetContext&, TArray<FCortexSTStateRef>&, bool bIncludeSelectorFields = true)`; retain `CollectStates` for single-root traversal. One visited set across all roots is seeded from existing output, skips null/repeated states, stops child cycles and preserves first ownership/order. Selectors use default GUID/path metadata; stored inspection passes false, avoiding off-page metadata and malformed Parent-chain walks. All addressable-state enumeration uses this shared owner.

- [x] Refresh candidate/base and plugin dirt; retain both contributor commits unchanged on a normal owning-plugin branch; no contributor scripts executed.
- [x] Map shared enumeration/selector consumers and root restrictions before edits.
- [x] Extend native fixtures with a second UStateTreeState subtree and child; exercise get_state, dump_tree, rename_state, and transition selectors. Compare exact GUID/name/edge rather than merely success.

```cpp
GetParams->SetStringField(TEXT("state_id"), LaterChild->ID.ToString(EGuidFormats::DigitsWithHyphens));
const FCortexCommandResult Result = Handler.Execute(TEXT("get_state"), GetParams);
TestTrue(TEXT("later subtree child is readable"), Result.bSuccess);
if (Result.bSuccess && Result.Data.IsValid())
{
    TestEqual(TEXT("selector returns the requested stored identity"),
        Result.Data->GetStringField(TEXT("id")), LaterChild->ID.ToString(EGuidFormats::DigitsWithHyphens));
}
```

- [x] Add null-first-slot and duplicate-path variants with exact semantic failures and non-mutation; use existing fixture cleanup.
- [x] Controller observe multi-root RED: 9/9 regressions fail on first-root-only selectors before production correction.
- [x] Move contributor enumeration to shared CortexST owner; remove duplicate local helper/prechecks; migrate read/mutation/transition consumers while retaining root restrictions.
- [x] Controller rebuild zero-warning and run StateTree GREEN 37/37; independent task-1 review passes without findings.

## Task 2: Stored inspection correctness and bounded work

**Files:** Operations/CortexSTInspectOps.cpp; CortexStateTreeCommandHandler.cpp; Tests/CortexStateTreeInspectTest.cpp, CortexStateTreeStoredInspectTest.cpp and private CortexStateTreeStoredInspectTestTypes.h. Capability fixture/generated fallback and toolkit guidance changed only for this inspection contract.

**Interfaces:** Preserve inspect_instances, inspect_section, inspect_offset, inspect_count and response schema. Internal section descriptors reference stored StateTree entries; total is counted without serializing all bodies. Defaults are offset 0/count 1; count 1..100, offset 0..INT32_MAX, finite integral numeric values only; offset==total yields an empty terminal page. Local stored compound traversal reuses Core leaf policies and carries branch-owned diagnostics; int64 outside the safe JSON range is decimal text and uint32 remains exact.

- [x] Add RED tests for invalid types/context/ranges, boundaries/reconstruction, stored fixed arrays/containers/optionals/scalars, lossless integers/GUID bindings and exact clean/dirty non-mutation.

```cpp
DumpParams->SetBoolField(TEXT("inspect_instances"), true);
DumpParams->SetNumberField(TEXT("inspect_section"), 7);
const FCortexCommandResult Invalid = Handler.Execute(TEXT("dump_tree"), DumpParams);
TestFalse(TEXT("malformed section cannot request an unpaged dump"), Invalid.bSuccess);
```

- [x] Controller observe StoredInspect RED 7/11 failures; correct a source-confirmed nonexported binding-copy fixture link error with reflected struct ops, not a production workaround.
- [x] Strictly parse present fields before reflection; paging controls require enabled inspection and named section, while inspect_instances=true alone remains unpaged.
- [x] Select section descriptors before reflecting stored values, preserving owner identity/completeness; no cache, retries, snapshot persistence or byte-budget API.
- [x] Complete fixed arrays/containers/optionals and exact numeric values with honest diagnostics; correct map-key diagnostic ownership and repeated/cyclic traversal after controller RED 2/13 failures.
- [x] Controller observe StateTree GREEN 48/48, then fresh integrated 51/51 and final 52/52. Actual stdio all-section acceptance compares known native values and independent native storage before/after, not self-comparison of inspection JSON.

## Integration and final version

- [x] Synchronize task-owned toolkit guidance and native capability fixture/generated fallback; canonical `--from-fixture --check` passes. Preserve unrelated toolkit gitlink changes.
- [x] Update accepted design/implementation and local StateTree system references; preserve owning-plugin plan copies and verification evidence/attribution.
- [x] Independently source-check review findings, observe focused RED before fixes, and run controller acceptance. Fresh source review resolves all original three findings and the new foreign-ownership finding.
- [x] Apply final descriptor bump (17/0.3.5 → 18/0.3.6) after behavior/documentation acceptance; descriptor JSON and exact values verified. Final exact-candidate review includes this metadata-only delta.
- Remote publication is outside this authorization. No push, merge, contributor-branch deletion, force operation or parent gitlink staging is claimed.

## Completed acceptance, review and durable rulings

### RED→GREEN

| Stage | Controller-observed result | Raw sandbox evidence |
|---|---|---|
| Submitted baseline | 28/28 native | `Saved/TestLogs/AutomationTest_2026-10-07_141012.log` |
| Shared selectors RED → GREEN | 9/9 fail → StateTree 37/37 pass | `AutomationTest_2026-10-07_142200.log` → `AutomationTest_2026-10-07_142738.log` |
| Stored inspection RED → GREEN | 7/11 fail → StateTree 48/48 pass | `AutomationTest_2026-10-07_145337.log` → `AutomationTest_2026-10-07_151621.log` |
| Diagnostic/cyclic-reference RED → fresh GREEN | 2/13 fail → StateTree 51/51 pass | `AutomationTest_2026-10-07_154827.log` → `AutomationTest_2026-10-07_171329.log` |
| Foreign fixture ownership RED → final GREEN | 1/1 fail → StateTree 52/52 pass | `AutomationTest_2026-10-07_180246.log` → `AutomationTest_2026-10-07_180835.log` |

All table basenames are under `Saved/TestLogs/`. Task-1 independent review passed; subsequent source-checked findings corrected ambiguous map-key diagnostic ownership, shared visited traversal and old-engine fixture APIs. A fresh reviewer identified foreign fixture admission, then approved exact Outer/ownership guards and five explicit alias scenarios after correction. Fresh implementer sessions were selected at the user's request rather than resuming stale sessions.

### Fixture and compatibility rulings

- Private Tests bridge only: `PrepareTopology`, `PopulateStoredInstances`, `CaptureSnapshot`; genuine native fixture, no production route/mock. Admission precedes Context assignment: Game Thread, exact case-sensitive loaded unsaved PR172-prefix tree, no file, EditorData Outer==tree, and cycle-safe reachable state/Parent/node-instance/runtime-object ownership.
- Five aliases cover blank/prepared foreign EditorData, foreign first/later roots and foreign child. Every utility refuses; owner/foreign full snapshots remain exact after restoring only injected aliases.
- Snapshots serialize native complete/no-delta bytes and compiled storage independently; no compile/PreSave/SavePackage/reference-modifying archive flags. Native tests compare clean and dirty states. Source initialization/availability guards support UE5.6/5.7 API differences without claiming unavailable local builds.
- GUID identity is native 32-bit reconstruction, not forced unsigned/case display. Asset paths retain established normalization, not a new object-path-only guarantee. Generic dirty fingerprints are not monotonic child-name tokens.
- Cleanup is targeted owned Python wrapper purge plus fresh-fingerprint deletion; no global UObject GC/new fallback. Task-owned editor exits via API; unrelated editors survive. Earlier failed-smoke autosave was separately owned/removed and is not evidence of inspection saving.

### Final observed scope

Supported UE5.8.3 build: succeeded, four actions/18.03s, zero compiler warnings. Native StateTree: 52/52, zero raw Warning:/Error:. Full rendering Cortex: 1738/1738 in 218.29s, zero raw warnings; one expected port-bind UE error and two expected Python RuntimeError output strings, not a raw-zero-error claim. Python isolated suite: 970 passed/265 deselected; no e2e/scenario/stress claim.

`Saved/PR172FinalMcpAcceptance.json` records `SMOKE_PASS` for actual stdio: initial single-factory-root unpaged capture, null-first-slot/later-root GUID/path/default and guarded rename, root/states/nodes/bindings count=1 pages (1/3/4/2 entries) plus terminal pages, known native typed values and real bindings, malformed-input refusal, independent exact snapshot/fingerprint/dirty/hash/compiled-storage/file equality. Remaining node families/depth cases, ambiguity/root restrictions are native-only coverage.

The shared40k formatter deliberately refused populated unpaged actual MCP twice (`RESPONSE_TOO_LARGE`, compact `_size=60682`, two policy warnings); full typed unpaged equivalence uses read-only TCP to the same verified editor PID30132/port8743. Entry bounds are not byte bounds; no formatter/API change. Bare fallback `--check` uses a Saved live cache and reported drift; fixture CI `--from-fixture --check` passed and unrelated live-cache domains were not regenerated.

Owning-plugin durable evidence: [PR172 verification report](../verification/2026-10-07-pr172.md). Build/Python/generator outcomes are controller-observed process evidence, not invented standalone log files. UE5.6/5.7/Linux builds/runtime and all-domain MCP benchmark were not exercised.

## Final closeout correction

Final reviewed candidate `33bdfdff4b3f841f90403ace5204edd5a52a506c` adds definitionless single-task paging preservation after valid RED at194957 and source-confirmed correction. Depth checkpoint6827 remains the historical StateTree53/53 and live stdio proof. The verification report supersedes earlier final counts/source and records rebuilt final integration acceptance, fresh source approvals, authorized publication, and the user's explicit two-animation-warning exception. Toolkit PR67 merged. Version18/0.3.6 unchanged; no unrelated animation changes.
