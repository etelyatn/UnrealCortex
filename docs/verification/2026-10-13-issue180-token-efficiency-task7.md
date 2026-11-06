# Issue 180 — Token efficiency, Task 7: ownership-safe inherited default delta

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `ec965e6` (Task 13 completion + its fix round) — the last committed HEAD when this
task's implementation was written. The implementation and its RED staging were subsequently committed
by the controller as `ebfbbdd` ("Task7: ownership-safe inherited default delta", on top of Task 16's
`c5df2e0`). The controller-confirmed review fix round recorded at the end of this document is
uncommitted on top of `ebfbbdd`.
**Owner:** Task 7 implementer (`ImplTask7Delta`) — re-application of the parked Core serializer
tests, the production implementation, the compile fix-up and this record. All builds, test runs and
smokes are controller-owned and are recorded here with their observed log identity.

## Scope

A new public serializer primitive plus one Blueprint read mode: `get_class_defaults` can now answer
"what does this Blueprint change relative to its parent class" instead of restating every effective
value. `changed_only=false` (the default) leaves every existing read byte-for-byte as before.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexCore/Public/CortexSerializer.h` (+16) | New public `FCortexSerializer::PropertyDifferenceToJson(Property, ValuePtr, BaselineValuePtr, Policy, FieldPath) -> FCortexPropertySerializationResult`, with its semantics documented. No previously private helper was exposed. |
| `Source/CortexCore/Private/CortexSerializer.cpp` (+101) | The helper's implementation. |
| `Source/CortexCore/Private/Tests/CortexSerializerDeepReadTest.cpp` (+299; now 536 lines) | Re-application of the parked Core delta tests (five `Cortex.Core.Serializer.Difference.*` tests). The file is byte-identical to the authoritative parked snapshot `task7-parked-core-file-snapshot.txt`, sha256 `183f29f572ea2b3c9159228acc1e3efb5f2e1076be1735848bd4c2a05967c19e`. |
| `Source/CortexBlueprint/Private/Operations/CortexBPClassDefaultsOps.cpp` (+202/-1) | `changed_only` control (shared parse), the delta branch in `GetClassDefaults`, two parent-baseline resolution helpers, two delta refusals, and the `ListDefaults` refusal. |
| `Source/CortexBlueprint/Private/CortexBPCommandHandler.cpp` (+1) | `changed_only` declared on the live `get_class_defaults` command info. |
| `README.md` (+2) | Generic Serialization documents the new helper and the delta view. |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task7.md` | This record. |

Staged by the controller before this implementation round (not written here): the consumer
regression `Cortex.Blueprint.TokenRead.Get.OwnedInheritedDelta` in
`Source/CortexBlueprint/Private/Tests/CortexBPTokenReadTest.cpp` and its dedicated native fixture
`Source/CortexBlueprint/Private/Tests/CortexBPTokenReadDeltaFixture.h` (`ACortexBPDeltaParentActor`,
`bReplicates=true`, `InheritedEqual=7`, `InheritedZero=5`, `InheritedText`, non-null
`InheritedObject`). Not part of this task: the generated MCP fallback/capabilities fixtures and the
toolkit/root Level documents — Task 17 owns those.

Fix round 1 (uncommitted on top of `ebfbbdd`; 5 files, +270/−4): the completeness classification and
the echo-level `echo_complete` field in `CortexBPClassDefaultsOps.cpp`, the `changed_only` command
description, the README delta-completeness sentence, the ten-type nested struct chain plus
`ACortexBPDeltaParentActor::Deep` in `CortexBPTokenReadDeltaFixture.h`, and the
`Cortex.Blueprint.TokenRead.Get.DepthLimitedDeltaIsNotEqual` regression (with its fixture-constructor
seed) in `CortexBPTokenReadTest.cpp`.

## Contract implemented

### `FCortexSerializer::PropertyDifferenceToJson(Property, ValuePtr, BaselineValuePtr, Policy, FieldPath)`

* Null property/pointer ⇒ `INVALID_PROPERTY` issue (error, omitted) and no value.
* `Property->Identical(ValuePtr, BaselineValuePtr)` ⇒ **no JSON value, no issues, not partial**. A
  value equal to its baseline is reported as *absent*, so a caller can drop it and record the
  omission explicitly.
* Differing value with `Policy.MaxDepth < 0` ⇒ `MAX_DEPTH_EXCEEDED` issue (error) + `bPartial` and no
  value. The historical `<changed>` sentinel is never produced by this API, so a depth-limited
  comparison can never read as a complete value.
* Plain `FStructProperty` ⇒ recursion over the members of **the same struct type** (the property's
  own `Struct` drives both sides, both pointers must address a value of that type in its own
  instance), `MaxDepth-1`, member admission still honoured; only members that differ land in the
  result object, so an equal member is omitted rather than restated. `FInstancedStruct`,
  `FGameplayTag`, `FGameplayTagContainer` and `FSoftObjectPath` stay atomic.
* Arrays, sets, maps, object identities and scalars ⇒ compared whole and serialized atomically
  through the existing `PropertyToJsonDeep`, whose issues and partial state are propagated
  (identity-only object reference, non-deterministic set order, partial text metadata, unsupported
  types). A legitimately null delta value (e.g. an object override to null) is kept as a value and is
  never mistaken for an omission.
* Field paths follow the existing `Base.Member` convention for structured diagnostics.

### `blueprint.get_class_defaults` `changed_only`

* `changed_only=false` (default) ⇒ unchanged: metadata discovery/paging, complete effective discovery
  read, and selected effective reads all behave exactly as before.
* `changed_only=true` (value mode, `value_mode=inherited_delta`):
  * Population is the same admitted root set as the existing discovery read
    (`ShouldExposePropertyForDiscovery`), so the delta view is a strict filter of the effective
    population; with selectors it is the resolved selector paths (nested paths included, exactly like
    the existing selected read).
  * Baseline per admitted root: a parent pointer is taken **only** when the parent class owns the
    very same field instance (`ParentProperty == RootProperty`), with the same property class
    (`SameType`), the same owner struct, the same `Offset_Internal`, and the parent class a descendant
    of that owner. For selector paths the same path is re-resolved on the parent CDO and must reach
    the same leaf instance and offset. Anything else is a subclass-owned (or shadowing) root: no
    parent pointer is taken at all, so a subclass field is never read through parent memory, and it
    is reported as its own full deep effective value.
  * Response: `value_mode="inherited_delta"`, `baseline_class` (same expression as the existing
    `parent_class` field), `effective_values_complete=false`, `omitted_equal_properties` (array of
    property-name strings, for equal inherited roots **and** for selected equal-to-baseline
    properties), `values_included=true`, `issues` only when the per-root serialization produced any
    (canonical `SerializationIssuesToJson` shape), and an honest `total_properties` for the unfiltered
    case (reported + explicitly omitted). `count` stays the number of entries in `properties`. A delta
    read is never paged.
  * `changed_only=true` with an explicit `include_values=false` refuses `INVALID_FIELD` before any
    asset load, compile or lookup. An unstated `include_values` cannot turn a delta into metadata: it
    resolves to values.
  * Completeness classification (review fix round 1): a root is listed in `omitted_equal_properties`
    only when its serialization was complete (no issue and not partial) **and** truly equal. A root
    whose comparison could not be completed — a depth-limited struct comparison returns a
    `MAX_DEPTH_EXCEEDED` issue with `bPartial` and no value — is never an omission: it keeps its
    entry, carries its own `issues`, is flagged `incomplete: true`, sets the new echo-level
    `echo_complete=false`, and presents **no** `value` (the entry builder is asked for values only when
    the comparison produced a usable one, so a failed comparison is never masked by an effective value
    or by a partial serialization's null placeholder). A usable but non-authoritative value (for
    example an FText descriptor without namespace/key metadata) is still presented and flagged; no
    existing field changes value or shape.
* No `NonDefaultPropertiesToJson`, no private serializer helper, and no member-expansion path is used
  by the delta view.

### Two fail-closed extensions beyond the design document (deliberate, documented)

1. `changed_only=true` also refuses `include_members=true` (`INVALID_FIELD`). Member expansion
   describes the *effective* struct members, which would contradict a delta that reports only the
   differing members; the combination refuses instead of mixing two meanings. The live command
   description and the README state the constraint.
2. `list_inherited_properties` / `list_settable_defaults` refuse `changed_only=true`
   (`INVALID_FIELD`). Both share the control parser but have no delta view; refusing prevents a
   silently ignored flag. They do not implement a delta.

## Controller-observed RED (before the implementation)

Filter `Cortex.Blueprint.TokenRead+`: 6 tests, **5 PASS / 1 FAIL**
(`Cortex.Blueprint.TokenRead.Get.OwnedInheritedDelta`), 12 assertion errors on 8 distinct
assertions. Log: `Saved/TestLogs/AutomationTest_2026-10-14_015334.log`.

The failure is exactly "the delta semantics do not exist yet, and the request was served as an
ordinary effective-value read":

* `delta declares its semantics` / `delta label` (test line 1024) — `value_mode` absent, read back as
  `""` instead of `"inherited_delta"`.
* `delta declares its baseline class` / `delta baseline names the parent class` (1029) —
  `baseline_class` absent; the fixture's parent class name was `…_DeltaParent_F5A58E75_C`.
* `delta declares effective completeness` / `a delta is never a complete effective-value read`
  (1035/1037) — `effective_values_complete` absent.
* `the equal inherited field is not a delta value` (1105) — `InheritedEqual` was returned as a value
  instead of being omitted.
* `delta lists omitted equal properties` (1106) — `omitted_equal_properties` absent.
* `changed_only with include_values=false should refuse` / `… should refuse with INVALID_FIELD`
  (1134) — the combination was accepted with an empty error code.
* `a same-value selected property is omitted from delta values` / `selected delta read lists omitted
  equal properties` (1155/1156) — the selected equal property was returned as a value and no omission
  list existed.

The remaining assertions of the same test already passed under the previous production behaviour:
fixture sanity (parent CDO replicates, child inherits `true` before the override and `false` after
it), the retained `OwnOnly=false` / `bReplicates=false` / `InheritedZero=0` /
`InheritedText="child text"` / `InheritedObject` values, `values_included=true`, and the plain read
still exposing `InheritedEqual`. The other five TokenRead tests passed unchanged (the delta read is
additive).

## Controller-observed GREEN

Both covering filters were run serially, one runner at a time, on the implementation:

| Filter | Result | Log |
|---|---|---|
| `Cortex.Core.Serializer+` | **52 tests, 52 PASS / 0 FAIL** | `Saved/TestLogs/AutomationTest_2026-10-14_021949.log` |
| `Cortex.Blueprint.TokenRead+` | **6 tests, 6 PASS / 0 FAIL** (including `Get.OwnedInheritedDelta`) | `Saved/TestLogs/AutomationTest_2026-10-14_022024.log` |

Raw severity scan of both run logs: clean. The only matches for a `Warning|Ensure|Fatal|Assertion`
scan are two unrelated boilerplate lines (`MapCheck: Map check complete: 0 Error(s), 0 Warning(s)`
and a `LogDesktopPlatform: Found project "…/WarningGateEvidence/…" in recently opened files`
recent-projects line). No unexpected diagnostic of any severity.

These two runs are for the **pre-fix revision** that became `ebfbbdd`; fix round 1 changes the delta
classification and adds a Blueprint test, so both filters must be re-run on the new revision and their
logs recorded here. The new expectation for `Cortex.Blueprint.TokenRead+` is 7 tests (the 6 above plus
`Get.DepthLimitedDeltaIsNotEqual`); `Cortex.Core.Serializer+` is unchanged at 52.

Builds (controller-owned, recorded here for provenance):

* `task-7-16-green-build2.log` — the successful build of the implementation: 0 errors, 0 warnings.
* `task-7-16-green-build.log` — the first attempt, which carried the single compile error below plus
  the three pre-existing deprecation warnings below.
* `task-7-16-red-build.log` / `task-7-16-red-build2.log` — the RED-staging builds.

## Compile fix-ups

1. **`error C2664` at `CortexBPClassDefaultsOps.cpp(587,44)`** (observed in `task-7-16-green-build.log`;
   fixed in this task): `ParentClass->FindPropertyByName(RootProperty->GetName())` cannot convert
   `FString` to `FName`. Fixed to `ParentClass->FindPropertyByName(RootProperty->GetFName())`
   (`FField::GetFName()` returns `FName`; `GetName()` returns `FString` and does not convert). No
   behaviour change. A sweep of the file found only one other property lookup, line 573
   `FindFProperty<FProperty>(GeneratedClass, FName(*RootName))`, already `FName`-correct; every other
   `GetName()` argument in the new code feeds `FString` parameters
   (`FJsonObject::SetField`/`SetObjectField`/`SetStringField`, `JoinFieldPath`, the new
   `const FString&` `Key`/`Selector`/`FieldPath` parameters), so no other site of that kind exists.
2. **Two RED-staging build repairs, recorded in `.superpowers/sdd/2026-10-13-token-efficiency-impl/
   progress.md` (line 166)** and folded into the staging that this implementation builds on: the
   untracked `UCLASS` fixture header had to leave the source tree while its constructor was still
   parked (UHT scanned it), and a `TSharedPtr<FJsonValue>*` test read needed a double deref. The
   consumer regression and the fixture header in the tree are the repaired form.
3. **Pre-existing deprecation warnings, not introduced here (provenance proven).** Full-recompile
   build logs report `warning C4996: 'StaticFindObject': StaticFindObject with a boolean ExactClass has
   been deprecated` at `CortexSerializer.cpp(1665,27)`, `(1701,29)`, `(1755,32)` in the pre-insertion
   log and at `(1766,27)`, `(1802,29)`, `(1856,32)` in the post-insertion log — the same three call
   sites, shifted by exactly the +101 lines this task inserted at line 802. `git show
   ec965e6:Source/CortexCore/Private/CortexSerializer.cpp` contains those three
   `StaticFindObject(..., false)` calls at 1665/1701/1755, i.e. they exist at the base revision and
   outside this task's added region. The final successful build (`task-7-16-green-build2.log`) and both
   run logs are free of them.

## Review fix round 1 (controller-confirmed P2)

Finding: the delta branch treated every absent serialized value as an equal-to-baseline omission, but
`PropertyDifferenceToJson` also returns no value when the comparison could not be completed — a
depth-limited struct comparison returns `MAX_DEPTH_EXCEEDED` with `bPartial` and no value. Such roots
were therefore falsely listed as equal omissions, which is a dishonest equality claim.

Fix (Blueprint delta branch only, additive):

* The omission path is gated on an authoritative result:
  `Serialized.Issues.Num() == 0 && !Serialized.bPartial && !Serialized.JsonValue.IsValid()`.
* A non-authoritative result keeps its entry, presents **no** `value`, is flagged `incomplete: true`,
  carries its own `issues` (canonical `SerializationIssuesToJson` shape), and clears the new
  echo-level `echo_complete` flag. The entry builder is asked for values only when the comparison
  produced a usable one, so a failed comparison cannot be masked by an effective value or by a partial
  serialization's null placeholder.
* A non-authoritative result that still produced a *usable* value keeps that value and is flagged the
  same way (for example an FText descriptor without namespace/key metadata, or an identity-only object
  reference); no existing field changes value or shape, so every staged assertion is untouched.
* `echo_complete` is always emitted in a delta read: `true` only when every examined root's
  serialization was authoritative.

Second pass of this round: the first pass asked the entry builder for values unconditionally, so an
entry whose comparison produced none still carried an effective value and the new regression's
`an incomplete comparison presents no value` assertion failed. The controller observed
`Cortex.Blueprint.TokenRead+` 6/7 with `…Get.DepthLimitedDeltaIsNotEqual` failing at
`CortexBPTokenReadTest.cpp:1287`; value extraction is now driven by the comparison's own result.

Regression added: `Cortex.Blueprint.TokenRead.Get.DepthLimitedDeltaIsNotEqual`. The fixture header
gains a ten-type nested struct chain (`FCortexBPDeltaLevel1`..`FCortexBPDeltaLevel10`, the innermost
holding `FString Leaf`) exposed as `ACortexBPDeltaParentActor::Deep`, seeded deterministically by the
fixture constructor. The child overrides only that innermost leaf through the production setter
(`Deep.Next…Next.Leaf`, nine `Next` segments) — ten struct levels below the CDO root property, past the
delta comparison depth of 8 — and the test asserts `echo_complete == false`, that `Deep` is **not** in
`omitted_equal_properties` while the genuinely equal `InheritedEqual` still is (and stays out of
`properties`), and that the `Deep` entry is present, flagged `incomplete`, carries no `value` field and
reports a `MAX_DEPTH_EXCEEDED` issue.

Expected observation for this round: `Cortex.Blueprint.TokenRead+` 7/7 and
`Cortex.Core.Serializer+` 52/52 (both re-run on the new revision), raw-severity clean; the controller
records the new logs.

## Behaviour preserved / pins ruled on

* Defaults and existing reads are unchanged: `changed_only=false` in the schema, metadata discovery
  paging, the complete effective discovery read, selected effective reads, `parent_class`, `count`,
  `values_included`, `include_members` and `ListDefaults`' own contract.
* `omitted_equal_properties` element shape is the property name (string), for both the unfiltered and
  the selected equal case; no metadata is attached to omission entries.
* `baseline_class` = `Blueprint->ParentClass->GetName()`, the same convention the existing
  `parent_class` field uses.
* Depth convention: `MaxDepth < 0` ⇒ structured `MAX_DEPTH_EXCEEDED` + partial + no value;
  `MaxDepth == 0` still compares the current level and lets the nested recursion report the issue.
  `MAX_DEPTH_EXCEEDED` is the existing deep-serializer code.
* No global GC, no disk assets, no shared package names: the delta test's dedicated packages are
  retired by the existing `FTokenReadScopedFixture` RAII, and the Core tests use transient
  `NewObject` instances marked garbage.

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` and `MCP/tests/fixtures/*` still describe
  `blueprint.get_class_defaults` without `changed_only`; regenerate from a live capabilities read.
  `MCP/src/tools/blueprint/class_defaults.py` has no `changed_only` parameter yet.
* Blueprint/Core system documentation beyond the plugin README entry (toolkit
  `resources/*.md`, root `docs/systems/*.md`) still describes the class-defaults read without the
  delta view; Task 17 owns that pass, including the two fail-closed refusals above.

## Not done here

No commit, no push, no toolkit/MCP edits, no changes to the staged consumer regression or its
fixture, no changes to any Frontend file. No assertion was weakened and no diagnostic was suppressed.
