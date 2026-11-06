# Issue 180 — Token efficiency, Task 5: selected DataAsset fields / material node properties

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `f27c48be6a90a83a02e0244ac62a023b15b5bbf3` (latest plugin commit when Task 5 was
staged; the Task 5 edits below are **uncommitted working-tree changes**)
**Owner:** Task 5 implementer (`Task5SelectedValues`) — RED test staging, then the production
implementation, the test-side diagnostics fix and this record; all builds, test runs and smokes are
controller-owned.

## Scope

`data.get_data_asset` gains an optional `fields` string array and `material.get_node` gains an
optional `properties` string array. Both select explicit property paths before serialization:
absent/empty keeps the existing full read, a selected read returns only the requested paths, and an
unknown path is refused instead of falling back to the full read. No global recursive
top-level-name admission filter is introduced.

Changed files (this task; all uncommitted at record time):

| File | Change |
|---|---|
| `Source/CortexData/Private/Operations/CortexDataAssetOps.cpp` | `+ #include "CortexPropertyUtils.h"`. `GetDataAsset` parses the optional `fields` array fail-closed and builds the read: absent/empty keeps `ObjectToJsonDeep`; otherwise each path is resolved with `FCortexPropertyUtils::ResolvePropertyPath(DataAsset, …)` and serialized with `FCortexSerializer::PropertyToJsonDeep(Property, ValuePtr, MakeReflectedReadPolicy(), FieldPath)` into a **path-keyed** `properties` object, aggregating only the selected values' issues/`partial`. |
| `Source/CortexData/Private/CortexDataCommandHandler.cpp` | `get_data_asset` operation metadata advertises the optional `fields` array and the omitted/unknown-path contract. |
| `Source/CortexMaterial/Private/Operations/CortexMaterialGraphOps.cpp` | `+ #include "CortexPropertyUtils.h"`. `GetNode` parses the optional `properties` array fail-closed; absent/empty keeps the existing `CPF_Edit` loop; otherwise each requested name is first gated on its **root segment** (the text before the first `.`, or the whole name) resolved with `Expression->GetClass()->FindPropertyByName` and admitted with the same flags (`CPF_Edit`, not `CPF_Transient`/`CPF_Deprecated`/`CPF_DisableEditOnInstance`), then resolved with `FCortexPropertyUtils::ResolvePropertyPath(Expression, …)` and re-admitted on the terminal leaf. `node_id` / `expression_class` / `position` / `collection_path` / `parameter_name` are produced by the untouched code paths. |
| `Source/CortexMaterial/Private/CortexMaterialCommandHandler.cpp` | `get_node` operation metadata advertises the optional `properties` array. |
| `Source/CortexData/Private/Tests/CortexTestDataAsset.h` | New dedicated fixture `UCortexTask5SelectedValuesAsset : public UCortexTestDataAsset` (`TestFlag=false`, `TestObjectRef=nullptr`, `FText TestLocalizedText`); the shared `UCortexTestDataAsset` used by the export/import/diff/search fixtures is unchanged. |
| `Source/CortexData/Private/Tests/CortexDataAssetCrudTest.cpp` | Three selected-read regressions (`Cortex.Data.DataAsset.GetSelectedFields`, `.GetSelectedFieldDiagnostics`, `.GetSelectedFieldUnknownRefusal`), all value reads typed/guarded. |
| `Source/CortexMaterial/Private/Tests/CortexMaterialGraphTest.cpp` | `+ #include "UObject/UnrealType.h"`. `Cortex.Material.Graph.GetNode.SelectedProperties` (now also pinning the unknown-property `PROPERTY_NOT_FOUND` code) and `Cortex.Material.Graph.GetNode.SelectedPropertiesExcludedRoot` (the review-fix regression below). |
| `.superpowers/sdd/2026-10-13-token-efficiency-impl/{task-5-report.md,smoke-task5-selected-values.py}` | Task coordination report and the throwaway public-route smoke (that directory is git-ignored; not part of any commit). |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task5.md` | This record. |

Not part of this task (deliberately): the MCP tool signatures/fallback, the capabilities cache and
the Data/Material documents — Task 17 owns those.

## Contract implemented

`data.get_data_asset`

* New optional param `fields` (array of strings). A present `fields` that is not an array of
  non-empty strings returns `INVALID_FIELD` (never a silent full read).
* Absent or empty `fields` keeps the complete deep reflected read byte-for-byte
  (`ObjectToJsonDeep` with the unchanged policy: `ReflectedRead`, text metadata on, depth 8,
  instanced sub-objects expanded).
* Otherwise each requested path is resolved against the asset root (dotted struct paths and
  non-null object paths are supported by `FCortexPropertyUtils::ResolvePropertyPath`). A path that
  does not resolve returns `PROPERTY_NOT_FOUND` with no `properties` payload — there is no
  full-read fallback.
* Each resolved value is serialized alone with the same deep policy and stored under the requested
  path string, so `properties` is the path-keyed object that class-default selected reads already
  use (`CortexBPClassDefaultsOps.cpp:930-936`). A selected `null`/`false`/`0` value is still
  emitted (the design's "every selected leaf value, including zero/false/null, is retained").
* `partial` and `issues` are aggregated from the selected values only, so an unselected unsupported
  property no longer contributes an issue and the requested unsupported property still reports its
  real `UNSUPPORTED_PROPERTY_TYPE` issue under the requested path.

`material.get_node`

* New optional param `properties` (array of strings). Present-but-not-an-array / non-string /
  empty entry returns `INVALID_FIELD`.
* Absent or empty keeps the current full read of every editable expression property.
* Otherwise only the requested names are serialized. A name's **root segment** (the text before the
  first `.`, or the whole name) must resolve on the expression and pass the loop's editable
  admission, and the resolved terminal leaf must pass the same admission; otherwise
  `PROPERTY_NOT_FOUND` is returned with no `properties` payload. The root gate is what stops a
  dotted selector from crossing a non-editable root into another object's editable fields.
* Values use the same `FCortexSerializer::PropertyToJson` call as the full loop, so a selected
  value is byte-identical to the full read's value for that property.
* Node identity (`node_id`, `expression_class`), `position` and the special
  `collection_path`/`parameter_name` selectors are produced outside the property loop and are
  therefore unchanged by selection.

## Deliberate decisions

1. **Material keeps `PropertyToJson`, not `PropertyToJsonDeep`.** The owner's full loop already uses
   `PropertyToJson` (`CortexMaterialGraphOps.cpp:259`), and `get_node` has no `partial`/`issues`
   surface. Reusing the same call keeps a selected value byte-identical to the full read and avoids
   inventing a new diagnostics contract for the material node read; design §3.1 TE05 says the
   material *loop* is restricted to the selected fields. `ResolvePropertyPath` (the brief's resolver
   reuse) is still used, which adds dotted-path support and exact refusal. Switching material to
   `PropertyToJsonDeep` would be a one-line change **plus** a new `partial`/`issues` contract, so it
   was not taken silently.
2. **Refusal codes.** Data refuses with `PROPERTY_NOT_FOUND` (`CortexErrorCodes::PropertyNotFound`),
   matching the adjacent class-default selected read. Material also uses `PROPERTY_NOT_FOUND` as the
   material-equivalent refusal; the material regression pins only `!bSuccess` plus "no fallback", so
   a ruling to material `INVALID_FIELD` would not require a test change.

## Controller-observed RED (before implementation)

* `Cortex.Data.DataAsset+` — 14 tests, 11 PASS / 3 FAIL (`Saved/TestLogs/AutomationTest_2026-10-13_235610.log`).
  The three new regressions failed because `fields` was ignored: the single-field read returned all
  13 reflected properties instead of 1; the mixed read all 13 instead of 5; the
  `Nested.Visible` dotted key was absent (empty value) while `Nested`/`NestedArray` were present;
  and the unknown root/leaf reads succeeded with a full payload instead of refusing.
* `Cortex.Material.Graph.GetNode+` — 4 tests, 3 PASS / 1 FAIL
  (`Saved/TestLogs/AutomationTest_2026-10-13_235704.log`). `GetNode.SelectedProperties` failed
  because `properties` was ignored: 5 returned keys instead of 1, `Desc`/`Collection` present, and
  the unknown property was not refused.

That RED run also produced exactly two `LogJson` warnings from the staged test itself
(`…_235610.log:2143-2144`, re-reported at `:2160-2161`): `Field Nested.Visible was not found.` and
`Json Value of type 'Null' used as a 'String'.` Root cause confirmed in engine source
(`Engine/Source/Runtime/Json/Private/Dom/JsonObject.cpp:338-357` and `:474-476`): `GetStringField`
is `GetField<EJson::None>(Name)->AsString()`, so a missing field logs the first message and the
`FJsonValueNull` it returns logs the second. Fix (test-side only, no assertion weakened, no
`AddExpectedError`, no suppression): every value read in the staged tests now uses
`TryGet{String,Bool,Number,Object,Array}Field` with `nullptr`/`IsValid()` guards before dereferencing
nested objects and `Type == EJson::String` before `AsString()`.

## Controller-observed GREEN

* `Cortex.Data.DataAsset+` — **14 tests, 14 Success**
  (`Saved/TestLogs/AutomationTest_2026-10-14_001454.log`): the 10 pre-existing
  `Cortex.Data.DataAsset.*` tests, the three new selected-read regressions, and
  `Cortex.Data.DataAssetSubclassFilter` (matched by the `DataAsset+` prefix).
* `Cortex.Material.Graph.GetNode+` — **4 tests, 4 Success**
  (`Saved/TestLogs/AutomationTest_2026-10-14_001559.log`): `GetNode.Basic`,
  `GetNode.CollectionParameter`, `GetNode.NotFound`, `GetNode.SelectedProperties`.
* Raw-severity scan of both GREEN logs: `LogAutomationController: (Error|Warning)` = 0,
  `Ensure|Assertion failed|Fatal` = 0, `LogJson` = 0. The only severity-keyword matches are the
  editor's optional-DLL load messages (`aqProf`/`VtuneApi`/`WinPixGpuCapturer`/`Wintab32`) and the
  engine's own `UE::UnifiedErrorTest` startup fixtures, both unrelated to this task and present in
  every run.

## Review fix (P2): material root-segment admission

Review `review-f27c48b..d88ebc8` found that the material branch applied the editable admission only
to the **terminal** resolved property, so a dotted selector crossing a non-editable root was
admitted. Concretely, `properties=["Material.TwoSided"]` resolved the expression's
`MaterialExpression::Material` — a plain `UPROPERTY() TObjectPtr<UMaterial>` with no edit flags
(`MaterialExpression.h:179-184`, public: the engine assigns it from other classes, e.g.
`Material.cpp:3624`) — and returned the owning material's editable
`UPROPERTY(EditAnywhere, Category=Material) uint8 TwoSided : 1` (`Material.h:559-561`) under that
key, although the full node read excludes `Material` entirely.

Fix (material branch only, `CortexMaterialGraphOps.cpp`): before traversing a requested name, its
root segment is resolved with `Expression->GetClass()->FindPropertyByName` and must pass the same
original admission flags (`CPF_Edit`, not
`CPF_Transient`/`CPF_Deprecated`/`CPF_DisableEditOnInstance`); the terminal admission is retained. An
excluded or missing root returns `PROPERTY_NOT_FOUND` with no `properties` payload. For a flat
selector the root segment is the name itself, so that path is unchanged. The Data path, the material
full/absent-selector behavior and the identity fields are untouched.

Regression: `Cortex.Material.Graph.GetNode.SelectedPropertiesExcludedRoot` — the full node read must
omit `Material`; the fixture pins the exact reviewed shape (root excluded by the admission, leaf
editable) by reflection on both properties; and `properties=["Material.TwoSided"]` must refuse with
`PROPERTY_NOT_FOUND` and no `properties` payload.

Fixture note (as required by the review): the engine populates `MaterialExpression::Material` when an
expression is loaded/duplicated (`MaterialExpressions.cpp:896`). A freshly created expression in the
same editor session does not necessarily have it yet, so the fixture explicitly establishes that
engine state (`if (Expr->Material == nullptr) { Expr->Material = Material; }`) and then **asserts it
is non-null**, so the refusal can only come from the root admission and never from a null object
pointer. No other non-editable-root/editable-leaf pair was needed.

Also pinned: the existing unknown-property regression now asserts the material refusal code
(`PROPERTY_NOT_FOUND`), matching the documented contract.

Controller observation for the fix: `Cortex.Material.Graph.GetNode+` (5 tests) and the unchanged
`Cortex.Data.DataAsset+` (14 tests). The new regression is expected **RED before the fix** (the
selected read succeeded and returned the material's `TwoSided`) and **GREEN after it**; this record
claims no runtime result for the fix, the controller owns the compile and the run.

## Registered smoke (controller-owned; NOT executed by the implementer)

`Plugins/UnrealCortex/.superpowers/sdd/2026-10-13-token-efficiency-impl/smoke-task5-selected-values.py`
(throwaway; the containing directory is git-ignored). It drives the shipped MCP stdio server and the
real registered `data_cmd` / `material_cmd` routers and asserts the brief's step-4 acceptance on the
public route:

* (a) `data_cmd list_data_assets` picks a saved project DataAsset (smallest qualifying full body);
  the full and selected bodies are printed in full, then asserted for: exactly the requested key,
  deep-equal value with the full read, identical `asset_path`/`asset_class`, and a strictly smaller
  compact body and `properties` object in characters **and** UTF-8 bytes. An empty `fields=[]` read
  must equal the full read exactly.
* (b) `material_cmd list_materials` + `list_nodes` pick a real expression node; the same
  equality/byte-delta proof runs plus `node_id`/`expression_class`/`position` identity, and an empty
  `properties=[]` read must equal the full read exactly.
* (c) An unknown `fields` path and an unknown `properties` name both return
  `success=false`/`_error="PROPERTY_NOT_FOUND"` with **no** `properties` key (no full-read fallback),
  and a malformed scalar selector returns `INVALID_FIELD`.
* Fixture policy: read-only against existing saved assets by default; only if the project exposes no
  suitable existing DataAsset/material node does the script create a disposable saved fixture
  through the registered `create_data_asset` / `create_material` + `add_node` contracts, prove the
  same contract on it, delete it in the same run and verify through `editor_cmd run_python` that the
  created `.uasset` files are gone.
* Decode policy: exactly one `TextContent`, `isError` false, `structuredContent` None, body is
  `json.loads` of the text (the flat public payload). `CORTEX_EDITOR_PID` is required and bound to
  the owned editor before any read. Exit 0 + `TASK5_SMOKE_OK` means every assertion held.

The smoke is controller-owned: it was syntax-checked (`py_compile`) but **not executed** here, and no
runtime result is claimed for it in this record.

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` and `MCP/tests/fixtures/capabilities_cache_full.json`
  still describe `get_data_asset`/`get_node` without `fields`/`properties`; regenerate after a live
  capabilities read. The MCP tool signatures `get_data_asset(asset_path)` and `get_node(asset_path,
  node_id)` are unchanged (the selectors are reachable through `data_cmd`/`material_cmd`), so
  `MCP/tests/fixtures/mcp_tool_signature_snapshot.json` and the Python tests
  (`MCP/tests/test_pagination_contract.py` reads `data.get_data_asset` for the oversize refusal
  only) need no change for this contract.
* Root `docs/systems/*data*`/`*material*` and toolkit
  `resources/mcp-tool-reference.md` still describe the reads without the selectors; Task 17 owns
  them.
* `MCP/src/cortex_mcp/response.py` already advertises the `fields`/`properties` recovery guidance for
  the oversize refusals (`_COMPLETE_OR_REFUSE_POLICIES`); no change needed.
