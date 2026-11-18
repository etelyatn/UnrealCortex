# Issue 226 implementation: honest writes, bounded reads, canonical codes and one MCP failure shape

Design: [2026-11-17-issue226-honest-writes-error-shapes-design.md](2026-11-17-issue226-honest-writes-error-shapes-design.md).

## Changes

- `Source/CortexCore/Private/CortexSerializer.cpp`, `Public/CortexSerializer.h`:
  - File-local helpers `AddJsonTypeMismatchWarning`, `AddJsonRangeWarning`, `AddEnumValueWarning`, `GetNumericJsonProbe`, `Fits*`, `TryResolveEnumJsonValue` and the RAII `FTemporaryPropertyValue` staging buffer.
  - `JsonToProperty` gates bool, int32, int64, float, double, byte, string, name, enum, `FGameplayTag`, `FSoftObjectPath`, object/class and soft object/class branches on the JSON type, and range-checks integers.
  - Array, map, `FGameplayTagContainer`, positional array-to-struct and `JsonToInstancedSubObject` writes convert into staged values and replace the live value only on success. The map branch checks the key `ImportText_Direct` and value results.
  - `PropertyToJson`/`StructToJson` are wrappers over private `PropertyToJsonInternal`/`StructToJsonInternal`, which carry the expanded-object path and depth. Public signatures are unchanged.
- `Source/CortexUMG`: `set_property`/`get_property` declared `StrictParams()` (`CortexUMGCommandHandler.cpp`). `CortexUMGWidgetPropertyOps.cpp` returns serializer warnings on success, and in the message and `details.warnings` on refusal.
- `Source/CortexLevel`:
  - `CortexLevelUtils.cpp` `TryParseVector`/`ParseVectorField` refuse a present non-array, wrong-length or non-number vector.
  - `SetTransform` validates all vectors before its transaction.
  - Rule 1 sweep: missing required parameters return `INVALID_FIELD` across Actor, Component, Discovery, Lifecycle, Organization, Query, Streaming and Transform ops.
  - Rule 3: create, duplicate and save level failures return `SAVE_FAILED`. Direct saves pass `SAVE_NoError`.
- `Source/CortexEditor`: `start_pie` declares only `mode`. `focus_node` returns `BLUEPRINT_NOT_FOUND`.
- `Source/CortexBlueprint`:
  - `add_scs_component` reports `compiled` from `IsBlueprintCompiled`.
  - `create`/`save` save failures return `SAVE_FAILED`.
  - `export_snapshot`/`export_text_inventory` return `BLUEPRINT_NOT_FOUND`.
  - Every package save passes `SAVE_NoError`. The `saved:false` settings commands add the failure to `warnings`.
- `Source/CortexGraph`:
  - `LoadBlueprint` returns `BLUEPRINT_NOT_FOUND`.
  - `SetPinValue` resolves its candidate default (class/object/interface via guarded load, soft references as paths) and calls `IsPinDefaultValid` before `Modify()`.
  - The text-pin save and `apply_patch` save pass `SAVE_NoError`. The text-pin save failure is `SAVE_FAILED`.
- `Source/CortexMaterial`: dynamic and parameter ops return `INVALID_FIELD` for a missing required parameter. A present but invalid `value` keeps `INVALID_PARAMETER`.
- `Source/CortexData`, `Source/CortexStateTree`: create/save failures return `SAVE_FAILED` with `SAVE_NoError`. `search_datatable_content` without `search_text` returns `INVALID_FIELD`.
- `Source/CortexGen`: missing job/prompt/source parameters return `INVALID_FIELD`.
- `Source/CortexAnimation`: `ResolveRequiredAsset<UAnimBlueprint>` returns `BLUEPRINT_NOT_FOUND`.
- `MCP/src/cortex_mcp/tool_output.py`:
  - New `canonical_failure` and `failed_step_code`.
  - `_wrap` converts a raised `UECommandError` or `ConnectionError`.
  - `_build_result` sets `isError` from `_error`.
- Routers, composites (Blueprint, Level, Material, UMG, StateTree, Gen, QA, Editor restart), schema, import queues, Reflect tools and the replay/response boundaries return `canonical_failure` instead of `Error:` text or `{error}` shapes.
- `MCP/tests/fixtures/capabilities_cache_full.json` and the regenerated `_fallback_generated.py` drop the `start_pie` parameters.
- `MCP/README.md` documents the failure shape.

## Tests

Native (all observed RED before the production change, GREEN after; logs under `Saved/Issue226/`):

- CortexCore, `Cortex.Core.Serializer.TypeSafety.*`:
  - `ScalarMismatch`, `NumericRange`, `NumericStringCoercion` (guard), `EnumOutOfRange`.
  - `MapRejectsInvalidKey`, `MapRejectsInvalidValue`, `ReferenceMismatch`, `GameplayTagContainerAllOrNothing`.
  - `InstancedCycleTerminates` (RED: stack overflow), `InstancedSameClassExpands` (guard).
- CortexCore, `Cortex.Core.Serializer.InstancedSubObject.ArrayFailurePreservesState`.
- CortexUMG:
  - `Cortex.UMG.SetProperty.MissingValue`, `UnknownEnumWarnings`, `StructFieldWarnings`.
  - `Cortex.UMG.GetProperty.SlotBounded` (RED: stack overflow).
- CortexLevel:
  - `Cortex.Level.Transform.SetTransformMalformedLocation`, `SpawnActorMalformedLocation`.
  - `Cortex.Level.ErrorCodes.MissingRequiredParams`.
  - `Cortex.Level.ErrorCodes.SaveFailure`, added after the RED run.
- CortexEditor: `Cortex.Editor.PIE.StartPIE.SchemaDeclaresOnlyMode`, `Cortex.Editor.FocusNode.BlueprintNotFound` (renamed from `AssetNotFound`).
- CortexBlueprint:
  - `Cortex.Blueprint.Component.AddSCSComponent.CompileFailure`.
  - `Cortex.Blueprint.Asset.SaveFailure` (RED: fatal `GError`).
  - `ExportSnapshot`/`ExportTextInventory` code pins.
- CortexGraph:
  - `Cortex.Graph.SetPinValue.InvalidDefaultValue`.
  - `Cortex.Graph.SetPinValue.Text.SaveFailure` (RED: fatal `GError`).
  - `ListGraphs`/`PatchCommand.MissingAsset` code pins.
- CortexMaterial: `Cortex.Material.Param.SetParameterMissingValue`.
- CortexData: `Cortex.Data.Search.MissingSearchText`, `Cortex.Data.Create.SaveFailure` (RED: fatal).
- CortexStateTree: `Cortex.StateTree.Asset.CreateSaveFailure` (RED: fatal).
- CortexGen: `Cortex.Gen.CommandHandler.ErrorCode.MissingField`.
- CortexAnimation: `Cortex.Animation.Resolve.MissingAnimBlueprintIsBlueprintNotFound`.

The save-failure tests refuse the save with `FCoreUObjectDelegates::IsPackageOKToSaveDelegate` or with a read-only directory at the package filename.

Python: `MCP/tests/test_error_shape_contract.py` (registrar `isError` rule and exception conversion, Reflect `suggestions`, router/composite/standalone canonical failures, `start_pie` schema). Existing composite/router tests now assert `_error`/`_message`.
