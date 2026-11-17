# Issue 222 implementation: undoable material writes and honest failures

Design: [2026-11-17-issue222-material-undo-design.md](2026-11-17-issue222-material-undo-design.md).

## Changes

- `Source/CortexMaterial/Private/Operations/CortexMaterialGraphOps.cpp`:
  - `AddNode`, `RemoveNode`, `SetNodeProperty` and `AutoLayout` hold `FScopedTransaction(Text, !IsInBatch())` at function scope. Each mutation runs between `CortexMaterialInputUtils::BeginEdit` and `EndEdit`.
  - `AddNode` creates the expression with `RF_Transactional`.
  - `RemoveNode` clears every expression input and MaterialResult input that points at the node. It calls `Modify()` on each changed expression, and on cleared root inputs it sets `UseConstant` the way `disconnect` does. It then calls `RemoveExpressionParameter` and removes the node from the collection.
  - `SetNodeProperty` parses into a staged copy, so a refused value returns before any edit. It commits with `Expression->Modify()` and `PreEditChange(Property)`/`PostEditChangeProperty`.
  - `AutoLayout` calls `Modify()` on each expression before it moves it.
- `Source/CortexMaterial/Private/Operations/CortexMaterialParamOps.cpp`:
  - `SetParameter` and `ResetParameter` open a transaction and call `Instance->Modify()` immediately before each write.
  - `SetParameters` refuses an empty array. It wraps all entries in one `Cortex: Set Material Parameters` transaction. When no entry succeeds it returns an error whose code is the first entry's code and whose details carry `success_count`, `total_count` and `errors`.
- `Source/CortexMaterial/Private/Operations/CortexMaterialAssetOps.{h,cpp}`: new `SaveNewAsset(Asset, PackageFilename, OutError)`. It saves with `SAVE_NoError` and returns `SAVE_FAILED` with `asset_path` and `filename` details. `CreateMaterial` and `CreateInstance` use it.
- `Source/CortexMaterial/Private/Operations/CortexMaterialCollectionOps.cpp`: `CreateCollection` uses `SaveNewAsset`.

## Tests

`Source/CortexMaterial/Private/Tests/CortexMaterialUndoTest.cpp` drives `FCortexMaterialCommandHandler` against in-memory materials and the editor transaction buffer. Each undo test first checks that the command's own transaction is the next undo step, so it never undoes an unrelated step.

- `Cortex.Material.Undo.AddNode`: Undo removes the added expression, and Redo restores it.
- `Cortex.Material.Undo.RemoveNode`: the BaseColor link and a Multiply `A` link to the removed Constant are cleared. Undo restores the node and both links, and Redo clears them again.
- `Cortex.Material.Undo.SetNodeProperty`: a `null` value is refused, with no change and no transaction. A valid value is undoable and redoable.
- `Cortex.Material.Undo.Parameters`: `set_parameter` and `reset_parameter` are undoable. `set_parameters` with two entries is one undo step.
- `Cortex.Material.Param.SetParametersAllFailed`: an all-failed batch returns `INVALID_PARAMETER` with an `errors` detail and records no transaction. An empty array is refused.
- `Cortex.Material.Asset.CreateSaveFailure`: a read-only directory at each target package filename makes `create_material`, `create_instance` and `create_collection` return `SAVE_FAILED`. The engine's refusal error and warning are declared as expected. The test removes the in-memory assets, the blocking directories and the temp files that the failed save leaves in `Saved/`.

## Behavior changes visible to callers

- Undo in the editor now reverts Cortex `add_node`, `remove_node`, `set_node_property`, `auto_layout`, `set_parameter`, `reset_parameter` and `set_parameters`.
- `remove_node` breaks links to the removed node.
- `set_parameters` with an empty array, or with no successful entry, now returns an error. A partial success still returns success with `errors`.
- `create_material`, `create_instance` and `create_collection` return `SAVE_FAILED` when the package cannot be written. The asset stays in memory.

## Verification

See [../verification/2026-11-17-issue222.md](../verification/2026-11-17-issue222.md).
