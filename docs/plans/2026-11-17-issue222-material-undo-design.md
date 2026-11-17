# Issue 222: Material graph and parameter writes must be undoable and must not report false success

Request: https://github.com/etelyatn/UnrealCortex/issues/222, reported by @XpycT from a source reading of `f72a0ad` (0.4.6).

## Outcome and boundary

Every CortexMaterial write that the issue names is one editor undo step that restores the previous state. `remove_node` leaves no link that points at the removed expression. A refused write leaves the asset and the undo history untouched. `set_parameters` and the `create_*` commands report failure when nothing was set or the save failed. There are no new commands or parameters. Two response contracts change: `set_parameters` with no successful entry, and a `create_*` whose save fails, now return errors.

## Source basis

At base `b388afb6` (0.4.11) every point in the report still held:

1. `add_node`, `remove_node` and `set_node_property` (`CortexMaterialGraphOps.cpp`) opened `FScopedTransaction` inside an `if` block. The transaction closed before the mutation, and nothing called `Modify()`, so Undo had nothing to restore. In a batch the outer batch transaction was open, but without `Modify()` it recorded nothing for these commands either.
2. `remove_node` only removed the expression from `ExpressionCollection`. MaterialResult inputs and other expressions' inputs kept pointing at it.
3. `set_node_property` called `Material->PreEditChange` and then returned without `PostEditChange` when `FCortexSerializer::JsonToProperty` refused the value. It never called the expression's own `PreEditChange`/`PostEditChangeProperty`.
4. `set_parameter` and `reset_parameter` (`CortexMaterialParamOps.cpp`) had no transaction and no `Instance->Modify()`.
5. `set_parameters` returned success with `success_count` 0. `create_material`, `create_instance` and `create_collection` ignored the `UPackage::SavePackage` result.

Found during the fix: `auto_layout` opened its transaction correctly but did not `Modify()` the expressions it moves, so Undo did not restore positions either. It is the same defect as point 1 and is fixed with it.

## Selected design

Reuse the established material edit pattern from `connect`/`disconnect`: `FScopedTransaction(Text, !FCortexCommandRouter::IsInBatch())` at function scope, with `CortexMaterialInputUtils::BeginEdit`/`EndEdit` around the mutation. `BeginEdit` calls `Modify()` on the material and its editor-only data. Outside a batch it also calls `PreEditChange`. `EndEdit` calls `PostEditChange`, or defers it to the batch, and then marks the package dirty. Expressions are `Modify()`d before they change.

- `add_node` creates the expression with `RF_Transactional`.
- `remove_node` mirrors the material editor delete. It clears every expression input and MaterialResult input that points at the node. A cleared root input gets the same `UseConstant` state that `disconnect` gives it. The node is then removed from the parameter map and the collection. The expression is not marked as garbage, so Undo can restore the same object.
- `set_node_property` uses the staged-value pattern from `set_material_property`: it parses into a copy first, so a refused value returns before any transaction or edit. The commit wraps the copy in the expression's `PreEditChange(Property)`/`PostEditChangeProperty`. When the serializer replaces or clears an instanced sub-object (for example a ColorRamp's `ColorCurve`), it marks the old sub-object as garbage while staging, outside any transaction. `set_node_property` revives every sub-object of the expression that staging retired. On success it retires them again inside the transaction, after `Modify()`, so Undo brings them back alive.
- `FCortexSerializer::JsonToProperty` (CortexCore) retires a replaced instanced value only when that value is owned by the `Outer` it was given. An instanced property can also hold an independent asset that was assigned by path. Before this change, clearing or replacing that value marked the asset itself as garbage, in every domain that writes through the serializer. Found by the independent review.
- `set_parameter`/`reset_parameter` open a transaction and `Modify()` the instance immediately before each write. A refusal leaves the transaction empty, and the engine discards empty transactions. `set_parameters` wraps all its entries in one outer transaction, so the batch is one undo step.
- `set_parameters` refuses an empty array (`INVALID_FIELD`). When no entry succeeds it returns the first entry's error code. The details carry `success_count`, `total_count` and `errors`. A partial success is still a success with `errors`.
- New `FCortexMaterialAssetOps::SaveNewAsset` saves with `SAVE_NoError`. On failure it returns `SAVE_FAILED` with `asset_path` and `filename`. All three `create_*` commands use it. The created asset stays in memory, so the caller can save it later or delete it.

Rejected: calling `UMaterialEditingLibrary::DeleteMaterialExpression`. It does not `Modify()` the other expressions whose inputs it clears, so their links would not come back on Undo, and it marks the expression as garbage.

## Acceptance

- After `add_node`, `remove_node`, `set_node_property`, `set_parameter`, `reset_parameter` and `set_parameters`, the command's transaction is the next undo step. Undo restores the prior state and Redo reapplies the change.
- `remove_node` of a Constant wired to BaseColor and to a Multiply input clears both links. Undo restores both links and the node.
- A `set_node_property` value that the serializer refuses changes nothing and records no transaction.
- `set_parameters` where every entry fails returns an error with per-entry `errors` and records no transaction. An empty array is refused.
- A `create_*` whose save fails returns `SAVE_FAILED`.
