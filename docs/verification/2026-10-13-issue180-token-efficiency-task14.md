# Issue 180 — Token efficiency, Task 14: changed-leaf class-default mutation echoes

**Date:** 2026-10-14 (issue-180 token-efficiency series; the record file keeps the series' 2026-10-13
prefix).
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `17b03bbc5ee8965c1a2170dce4bdf4a56fe3dfcd` ("Task7 fix: an incomplete comparison is not
an equal omission") — the committed HEAD when this task's implementation was written and when both
GREEN runs were observed. The production changes (`CortexBPClassDefaultsOps.cpp`,
`CortexBPCommandHandler.cpp`) and the staged regressions are **uncommitted** working-tree changes. Task 8
was subsequently committed as `081148f9` on the same branch (17b03bbc is its ancestor), leaving this
task's changes still uncommitted.
**Owner:** Task 14 implementer (`ImplTask14Echo`) — production implementation, the two fix passes and
this record. The RED regressions and their fixture were staged in the preceding RED phase. All builds,
test runs and smokes are controller-owned and are recorded here with their observed log identity.

## Scope

`blueprint.set_class_defaults` now reports a mutation echo whose representation depends on an explicit
`echo` selector, without changing what is applied, saved, compiled or recovered. The private exact native
snapshot plus the local exact-decimal integer encoding make the echo and the rollback honest for
precision-sensitive roots that the legacy JSON double encoding cannot represent. No global
serializer/setter change, no second public Core diff API, no cross-domain echo framework.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexBlueprint/Private/Operations/CortexBPClassDefaultsOps.cpp` | `+724/-9` (2505 lines). File-local echo helpers (`EClassDefaultEchoMode`, `PropertyTreeHasWideInteger`, `SerializeEchoValueExact`, `JsonValuesEqual`, `DiffStructLeavesJson`, `DiffStructLeavesNative`, `FCortexClassDefaultValueSnapshot`, `ApplyEchoEncodings`, `BuildClassDefaultEchoDiff`, `ParseClassDefaultEchoMode`) and the `SetClassDefaults` wiring: `echo` parse, snapshot capture before assignment, mode-appropriate echo, exact-snapshot rollback. |
| `Source/CortexBlueprint/Private/CortexBPCommandHandler.cpp` | `+2/-1`. `set_class_defaults` advertises the optional `echo` selector (and names it in the batch-item description). |
| `Source/CortexBlueprint/Private/Tests/CortexBPClassDefaultsTest.cpp` | `+1040/-0` — **staged in the RED phase**, unchanged by this task (5 `Cortex.Blueprint.ClassDefaults.Set.Echo*` regressions plus their fixture helpers). |
| `Source/CortexBlueprint/Private/Tests/CortexBPTokenReadTest.cpp` | `+67/-0` — **staged in the RED phase**, unchanged by this task (`Cortex.Blueprint.TokenRead.Set.NoEchoKeepsAuthoritativeOutcome`, re-applied at the recorded anchor after Task 7's commit). |
| `Source/CortexBlueprint/Private/Tests/CortexBPEchoContractFixture.h` | New (86 lines) — the dedicated echo fixture (`FCortexBPEchoPayload`, `ACortexBPEchoContractActor`); staged in the RED phase, unchanged. |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task14.md` | This record. |

Not part of this task (deliberately): the generated MCP fallback and capabilities fixtures and the
toolkit/root documents — Task 17 owns those.

## Contract implemented

### `blueprint.set_class_defaults` `echo`

* Optional `echo` string, validated **before any mutation**: `diff` (default) | `full` | `none`. A
  non-string or unknown value refuses `INVALID_FIELD`, so a malformed selector never leaves a partial
  write. The single-target path, the single `expected_fingerprint` path and per batch item all parse it
  (batch items already forward every item field except `target`/`expected_fingerprint`, so per-item
  `echo` is admitted by the existing preflight unchanged).
* Every per-property record carries `type`, `success` and `changed` in **all three modes**.
* `diff` (default):
  * scalar/atomic root — `previous_value`/`new_value` present **only when `changed`**; an unchanged root
    emits neither.
  * struct root — a `changes` object keyed by the **relative leaf path** (`X`, `bFlag`, `Payload`,
    `Big`; dotted for nested structs), each entry `{previous_value, new_value}`, and the struct root
    carries **no** complete `previous_value`/`new_value`. Unchanged siblings stay out; a no-op write emits
    no value echo at all.
* `full` — complete `previous_value`/`new_value` for every requested property, plus `changed`.
* `none` — no `previous_value`/`new_value`/`changes`; `type`/`success`/`changed` (the authoritative
  outcome) survive.
* All three modes apply identical values, report identical `compiled`/`saved`, warnings, guards and
  per-item state, and produce the same failure recovery: a later-property failure returns the same
  `PROPERTY_NOT_FOUND` with the same applied-value rollback.

### Comparison

* Non-precision roots compare through a deep JSON walk (`JsonValuesEqual`) over the frozen legacy JSON
  snapshots; strings compare **case-sensitively** (`FString::Equals(..., ESearchCase::CaseSensitive)`),
  `FText` compares through its descriptor object, and arrays/sets/maps compare atomically (whole value).
* Precision-sensitive roots never use the rounded JSON comparison: `PropertyTreeHasWideInteger` marks any
  reflected tree (struct / array / set / map / enum-underlying) that can hold an `int64`/`uint64` leaf,
  and those roots are compared with native `FProperty::Identical` (exact for 64-bit integers).

### `FCortexClassDefaultValueSnapshot` (private)

* Constructed **before assignment** for every precision-sensitive root — in every echo mode, because the
  rollback needs it regardless of the echo. Owned storage from
  `FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment())`, initialised with
  `FProperty::InitializeValue`, copied with `CopyCompleteValue`, released with `DestroyValue` — the real
  property's field type/offset/size/alignment, never a class-sized or parent-CDO-layout buffer.
* Derives `FGCObject`; reachability is held as **`TObjectPtr<UObject>` handles** — a
  `TObjectPtr<UObject> Owner` for the CDO plus a `TArray<TObjectPtr<UObject>> ReferencedObjects` gathered
  once from the frozen copy over the same reflected struct/container tree. `AddReferencedObjects` passes
  those handles to `FReferenceCollector::AddReferencedObject` so the handle-aware overload is selected
  (safe under incremental GC). The snapshot is immutable after capture, so the reference set is stable.
* `changed` is decided with native `FProperty::Identical` whenever the snapshot is present.
* Rollback prefers `Snapshot->RestoreTo(ValuePtr)` (`CopyCompleteValue` back onto the live value pointer);
  every other root keeps the existing legacy-JSON journal. The snapshots live only inside the
  `FScopedTransaction` scope (`TUniquePtr` in the applied-change journal) and are released **before** the
  explicit compile/reinstance — no snapshot storage pointer outlives the mutation.

### Exact-decimal integer echo (local)

* Integer echo serialization follows `CortexSTInspectOps.cpp:113-121` implemented **locally** (no
  StateTree dependency): `int64`/`uint64` leaves outside `[-9007199254740991, 9007199254740991]` are
  emitted as exact decimal strings.
* A precision scalar root reports `value_encoding="decimal_string"`; struct/atomic-collection roots
  report `value_encodings` (relative leaf path → `"decimal_string"`). Inside the exactly representable
  range the legacy number form is kept with no encoding marker.
* Those decimal strings are never fed back into the legacy JSON setter (`JsonToProperty`); only the exact
  native snapshot restores precision-sensitive roots.

## Controller-observed RED (before the implementation)

| Filter | Result | Log |
|---|---|---|
| `Cortex.Blueprint.ClassDefaults+` | 27 tests, **22 PASS / 5 FAIL** — the five `Set.Echo*` regressions | `Saved/TestLogs/AutomationTest_2026-10-14_025553.log` |
| `Cortex.Blueprint.TokenRead+` | 8 tests, **7 PASS / 1 FAIL** — `Set.NoEchoKeepsAuthoritativeOutcome` | `Saved/TestLogs/AutomationTest_2026-10-14_025626.log` |

The five ClassDefaults failures were exactly the missing `changed`/`changes`/`value_encoding`, the
unconditional full echo, and the lossy precision rollback; the TokenRead failure was the still-emitted
`previous_value` under `echo=none`.

## Fix passes

### Pass 1 — comparison completeness (Task 7's rule, applied at implementation time)

Task 7's record (`2026-10-13-issue180-token-efficiency-task7.md`, review round 1) established that *an
incomplete comparison is never an equal omission*: a comparison that could not be completed must not be
reported as equality. The class-default echo comparison inherits that rule and was made fail-closed:

* `JsonValuesEqual` compares JSON strings with `FString::Equals(..., ESearchCase::CaseSensitive)`.
  `FString::operator==` is case-insensitive in Unreal, so the first form would have reported the case-only
  `CaseText "abc" → "ABC"` change — and the changed literal inside the `FText` descriptor — as an equal
  omission, i.e. a false equality claim.
* A side that is missing/invalid is never equal: `JsonValuesEqual` returns `Left.IsValid() ==
  Right.IsValid()`, so a comparison that cannot be made reports `changed=true` instead of omitting the
  value.
* Precision-sensitive roots are not compared through the rounded JSON at all — `PropertyTreeHasWideInteger`
  routes them to native `FProperty::Identical`, so an incomplete (rounded) representation can never claim
  equality.

### Pass 2 — C4996 collector deprecation (after the first implementation build)

The first implementation build succeeded but reported two new deprecations from the snapshot's
`AddReferencedObjects` (`FReferenceCollector::AddReferencedObject` — "randomly crash if this function is
called when incremental gc is enabled; pass `TObjectPtr<...>`"), at
`CortexBPClassDefaultsOps.cpp(1198,15)` and `(1221,16)`
(`.superpowers/sdd/2026-10-13-token-efficiency-impl/task-8-14-11-build.log`, lines 24-28). Fixed without
suppressing the warning: the owner is now `TObjectPtr<UObject> Owner`, object references are gathered once
into `TArray<TObjectPtr<UObject>> ReferencedObjects` via `FObjectPropertyBase::GetObjectPtrPropertyValue`
along the same reflected tree, and `AddReferencedObjects` passes those `TObjectPtr` lvalues so the
handle-aware overload is selected (identical reachability set; the two raw calls are gone). Sweep of the
remaining new code found no other deprecated call.

## Controller-observed GREEN

Both covering filters were run serially, one runner at a time, on the post-fix implementation:

| Filter | Result | Log |
|---|---|---|
| `Cortex.Blueprint.ClassDefaults+` | **27 tests, 27 PASS / 0 FAIL** (including the five `Set.Echo*`) | `Saved/TestLogs/AutomationTest_2026-10-14_033111.log` |
| `Cortex.Blueprint.TokenRead+` | **8 tests, 8 PASS / 0 FAIL** (including `Set.NoEchoKeepsAuthoritativeOutcome`) | `Saved/TestLogs/AutomationTest_2026-10-14_033142.log` |

Raw severity scan of both run logs: clean — 0 `LogAutomation` errors/warnings, 0 `Ensure`, 0 `Fatal`,
0 `Assertion failed`. The only `Error`/`Warning` matches are unrelated boilerplate (optional
`*Prof.dll`/`VtuneApi*.dll`/`WinPixGpuCapturer.dll`/`Wintab32.dll` load failures, the engine's own
`UE::UnifiedErrorTest` `LogTemp` lines, and `MapCheck: … 0 Error(s), 0 Warning(s)`).

## Builds (controller-owned, recorded here for provenance)

* `task-14-8-red-build.log` — RED-staging build (CortexBlueprint.4), Succeeded.
* `task-8-14-11-build.log` — first implementation build, Succeeded; the two new C4996 snapshot collector
  deprecations above.
* `task-14fix-11-build.log` — post-fix build (7 actions, CortexBlueprint + CortexEditor), Succeeded, no
  warnings shown.

## Registered smoke (controller-owned)

A live `blueprint.set_class_defaults` smoke covering the real write/read and explicit save/cold reload for
each echo mode, and the save/compile failure path retaining the same applied/rollback outcome, is
controller-owned and is **not claimed here**. The contract this record describes is otherwise fully
covered by the two GREEN filters above.

## Behaviour preserved / pins ruled on

* `diff` is the default; an absent `echo` behaves as `diff`, so every existing caller's mutation outcome
  (`compiled`/`saved`/`success`, applied values, guard refusals, `PROPERTY_NOT_FOUND` recovery) is
  unchanged. Only the value representation narrows.
* `changed` is present in every mode and is never derived from a rounded representation: precision roots
  use native `Identical`, and a comparison that cannot be made is treated as changed, never as equal.
* A genuinely unchanged property (value equal to its previous) reports `changed=false` with no value echo
  in `diff`, and still presents the complete pair in `full`.
* The legacy JSON journal is retained in full and still drives rollback for every ordinary root; the exact
  snapshot is additive only for precision-sensitive roots.
* No global GC, no suppression, no weakened assertion: the staged regressions are byte-unchanged from the
  RED phase, and the fixture retires only its own dedicated package.

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` and `MCP/tests/fixtures/*` (capabilities cache) do not yet
  describe the `echo` selector for `blueprint.set_class_defaults`; regenerate from a live capabilities
  read.
* The toolkit/root class-defaults documentation (toolkit `resources/class-defaults*.md`, root
  `docs/systems/*class-default*`) does not describe `echo=diff|full|none`, the changed-leaf `diff` shape,
  or the precision `value_encoding`/`value_encodings` metadata; Task 17 owns that pass.
* No Python consumer asserts `previous_value`/`new_value` on `set_class_defaults`
  (`MCP/tests/test_class_defaults.py`, `MCP/tests/test_e2e.py` forward params only), so no Python test
  change is required by this contract — but the forwarded-tool schema should expose `echo`.

## Not done here

No commit, no push, no toolkit/MCP edits, no changes to the staged regressions or their fixture, no
changes to any unrelated domain. No assertion was weakened and no diagnostic was suppressed.
