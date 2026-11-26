# Issue 265 design: material function graphs

Issue: https://github.com/etelyatn/UnrealCortex/issues/265.

## Problem

Every graph command in `CortexMaterialGraphOps.cpp` loads a `UMaterial` through
`FCortexMaterialAssetOps::LoadMaterial`, so a material function path answers `MATERIAL_NOT_FOUND`.
Shared shading logic lives in material functions; they must be readable and editable through the same
graph commands.

## Engine facts the design relies on (UE 5.8)

- `UMaterialFunction` stores expressions in `UMaterialFunctionEditorOnlyData::ExpressionCollection`
  (`ExpressionCollection.Expressions`), the same `FMaterialExpressionCollection` a material uses.
  `UMaterialFunction::GetExpressions()` and the virtual `UMaterialFunctionInterface::GetExpressions()`
  return a `TConstArrayView<TObjectPtr<UMaterialExpression>>`; on a `UMaterialFunctionInstance` the
  virtual resolves the base function's expressions.
- Material function inputs and outputs are ordinary expressions in that collection:
  `UMaterialExpressionFunctionInput` (property `InputName`, `Preview` input) and
  `UMaterialExpressionFunctionOutput` (property `OutputName`, input `A`). There is no virtual result
  node: `FunctionOutput` expressions are the real sinks, and `UMaterialFunction::GetInputsAndOutputs`
  derives the callable signature from them.
- `UMaterialExpressionFunctionOutput::GetInputName` overrides the reflected default with `NAME_None`,
  so its single input is not addressable through `GetInputName`. Reflection (`FExpressionInput`
  struct properties, the same walk `UMaterialExpression::GetInputName` uses) yields the stable pin
  name `A`.
- `UMaterialExpressionFunctionInput::IsAllowedIn` / `UMaterialExpressionFunctionOutput::IsAllowedIn`
  are true only for `UMaterialFunction`; the material editor never places them in a material.
- `UMaterialEditingLibrary::UpdateMaterialFunction(UMaterialFunctionInterface*, UMaterial*)`
  (module `MaterialEditor`, already a private dependency of `CortexMaterial`) is the editor-owned
  "after edits" call: it regenerates `StateId`, calls `UpdateInputOutputTypes` and
  `UpdateDependentFunctionCandidates`, recompiles every loaded material/instance that depends on the
  function through an `FMaterialUpdateContext`, and marks packages dirty.
- `UMaterialFunctionInstance` has no expressions of its own (`GetExpressions` resolves the base
  function) and no graph of its own. Its overrides live in `ScalarParameterValues`,
  `VectorParameterValues`, ... and are read through
  `GetParameterOverrideValue(EMaterialParameterType, FName, FMaterialParameterMetadata&)`.
  `UMaterialExpression::GetParameterValue` supplies the parameter type and name for a node.
- Function layout is a pure data-flow graph: `FunctionOutput` nodes are real sinks, so the existing
  shared layout engine needs no virtual sink. `UMaterialFunction::ForceRecompileForRendering` is not
  a layout operation; moving nodes must not regenerate `StateId` or recompile dependents.

## Design

### Target resolution

New private `FCortexMaterialGraphTarget` (`Source/CortexMaterial/Private/Operations/`) resolves one
graph asset path to exactly one of:

| Kind | Accepted class | Writes |
|---|---|---|
| `material` | `UMaterial` | yes |
| `material_function` | `UMaterialFunction` | yes |
| `material_function_instance` | `UMaterialFunctionInstance` with a base function | no |

Resolution checks the package first (`FindPackage` / `DoesPackageExist`) and then loads a
`UObject` once; a path that is not one of the three classes refuses with `MATERIAL_NOT_FOUND` and a
message naming the accepted classes. A function instance without a base function refuses the same
way. The target exposes: kind name, asset object, expressions view, expression lookup, root-input
resolution (materials only), edit begin/end, and `Recompile`.

`FCortexMaterialAssetOps::LoadMaterial` stays unchanged for asset/parameter/collection operations;
only the graph commands move to the target loader.

### Graph surface

- `list_nodes`: expressions of the target; `MaterialResult` is appended only for a material.
  Response gains `asset_kind` (`material`, `material_function`, `material_function_instance`).
- `get_node`: any expression of the target; response gains `asset_kind`. On a function instance a
  parameter expression additionally reports its effective override as
  `parameter_override: {name, type, value}`. `type` is one of `scalar`, `vector`, `double_vector`,
  `texture`, `texture_collection`, `runtime_virtual_texture`, `sparse_volume_texture`, `font`,
  `static_switch`, `parameter_collection`, `static_component_mask`; `value` uses the existing JSON
  shapes (number, `{r,g,b,a}`, `{x,y,z,w}`, object path, bool, `{r,g,b}` mask, `{font,page}`). The
  field is absent when the instance does not override the node's parameter.
- `list_connections`: edges between expressions for every kind; `MaterialResult` edges only for a
  material; response gains `asset_kind`. `include_input_state` is material-only (refused with
  `INVALID_OPERATION` otherwise) because root input state lives on `UMaterial`, not on a node.
- `get_node_pins`: every kind; unnamed expression inputs report their reflected pin name (`A` for a
  function output) instead of an empty string; response gains `asset_kind`.
- `add_node`: every writable kind. New expression outer is the target object. Before creation the
  class default's `IsAllowedIn(TargetAsset)` is checked, so function-only nodes are refused in a
  material. Function input/output nodes get `ConditionallyGenerateId(true)` as the material editor
  does, and their signature names are made unique against the function's expressions (the engine's
  `ValidateName` only scans a `UMaterial`, so it is a no-op for function-owned nodes).
- `remove_node`: every writable kind. Links from every other expression are cleared; material root
  links and material parameter registration happen only for a material. Then the expression leaves
  the target's expression collection.
- `connect`: every writable kind. Source/target expression validation and root `MaterialResult`
  handling are unchanged; target pin names use the reflected fallback above.
- `disconnect`: every writable kind. Expression-to-expression disconnect is now supported (clear the
  addressed `FExpressionInput`), and the material root path is unchanged.
- `set_node_property`: every writable kind; recompiles a function after the property write. Renaming a
  function input/output to a name another signature node already uses refuses with `INVALID_VALUE`
  before staging (the editor would open a modal dialog instead).
- `auto_layout`: every writable kind. Material keeps the virtual `MaterialResult` sink and anchor;
  a function lays out its real expressions (function outputs are sinks) with no virtual node and no
  recompile.
- `set_material_input`: materials only; a function or function instance refuses with
  `INVALID_OPERATION` before any lookup or mutation.

Function instances are read-only: `add_node`, `remove_node`, `connect`, `disconnect`, `auto_layout`,
`set_node_property` and `set_material_input` refuse with `INVALID_OPERATION` and name the base
function. Reads resolve the base function's graph.

### Transactions, dirty state and recompile

- Begin edit: `Modify()` the asset and its editor-only data (`RF_Transactional`), and
  `PreEditChange(nullptr)` outside a batch, mirroring the material path.
- End edit for a material is unchanged.
- End edit for a function expression/connection/property write:
  - outside a batch, call `UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr)`;
  - inside a batch, register one deduplicated `FCortexBatchScope::AddCleanupAction` keyed by the
    function path, so a batch recompiles once at the end.
- `auto_layout` on a function only marks the package dirty and calls `PostEditChange()`; it never
  regenerates `StateId` or recompiles dependent materials.
- After a function graph write, the open function editor is refreshed from the asset on the next
  tick (`UMaterialEditingLibrary::RefreshMaterialFunctionEditor`, deduplicated per function). The
  refresh must be deferred: `RefreshGraphFromOriginal` no-ops while `GIsTransacting`, and every
  command wraps its edit in a transaction. Without it, the editor's stale duplicate is copied back
  over the asset on Apply.
- Function instances never reach begin/end edit.

### Errors

| Situation | Code |
|---|---|
| Missing package / not a material graph asset / instance without base | `MATERIAL_NOT_FOUND` |
| Unknown node id | `NODE_NOT_FOUND` |
| Unknown or unavailable pin / input | `INVALID_CONNECTION` |
| Write to a function instance | `INVALID_OPERATION` |
| `set_material_input` or `include_input_state` on a non-material | `INVALID_OPERATION` |
| Expression class not allowed in the target | `INVALID_OPERATION` |

## Non-goals

- Creating material function or function instance assets (use `core.save_asset` for persistence).
- Editing instance parameter overrides or adding parameter commands for functions.
- Expanding/rebuilding material function call expressions; `UpdateMaterialFunction` owns recompiles.
- Adding material functions to `list_materials` / `get_material` (issue scope is graph commands).

## Tests

`Source/CortexMaterial/Private/Tests/CortexMaterialFunctionGraphTest.cpp`
(`Cortex.Material.Graph.Function.*`), each with a unique in-memory function package:

| Test | Proves |
|---|---|
| `ListNodes` | Function graph read succeeds; no virtual `MaterialResult`; `asset_kind` |
| `GetNodePinsAndConnections` | Function output pin is `A`; connect/list/get_node_pins/get_node work |
| `WriteAndRecompile` | add/remove/set_node_property mutate the function, change `StateId`, and preserve the expression collection |
| `Disconnect` | expression-to-expression disconnect clears the pin |
| `AutoLayout` | function nodes are repositioned without a virtual sink |
| `Instance.ReadOnlyAndOverride` | base graph reads, `parameter_override` value, write refusal with no mutation |
| `MaterialOnlyOperationsRefused` | `set_material_input` and `include_input_state` refuse on a function |
| `UndoAddNode` | add_node is undoable on a function |
| `RemoveNodeBreaksLinks` | removing a source clears the function output's input |
| `UniqueSignatureNames` | added outputs/inputs get editor-style suffixes; duplicate renames refuse `INVALID_VALUE` and keep names |
| existing `Cortex.Material.Graph.*` | material behavior unchanged |

The live smoke exercises the same public route against a real project material function plus a
temporary function instance, and verifies the actual editor-side effect before saving.
