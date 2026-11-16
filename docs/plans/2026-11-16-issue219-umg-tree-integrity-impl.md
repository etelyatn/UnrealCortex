# Issue 219 implementation: UMG tree integrity

Design: [2026-11-16-issue219-umg-tree-integrity-design.md](2026-11-16-issue219-umg-tree-integrity-design.md).

## Changes

- `Source/CortexUMG/Private/CortexUMGUtils.h`
  - New `WidgetNameHeldByOtherClass`: a detached object outered to the `WidgetTree` (`StaticFindObjectFast`) with a different class blocks the name. `add_widget` and `duplicate_widget` (per planned name and source class) check it next to `WidgetNameExists`.
  - New `ModifyWidgetForEdit(WBP, Widget)`: journals the asset, the widget and its slot.
- `Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp`
  - `AddWidget`: root/parent/panel/`CanAddMoreChildren`/slot validation moved ahead of `ConstructWidget`. `slot_index` in the response is the actual child index.
  - `RemoveWidget`: journals tree, asset, parent, slot and subtree; `UWidgetTree::RemoveWidget`; renames the subtree into the transient package and calls `OnVariableRemoved` for each name.
  - `Reparent`: ancestor walk refuses self/descendant/root moves; `CanAddMoreChildren` and `slot_index` range checks; no explicit pre-detach (`AddChild` detaches after its own capacity check); journals asset, widget, old slot, old and new parent.
  - `DuplicateWidget`: refuses a parentless source or a full parent; validates every planned name before constructing; journals asset, tree and parent.
- `Source/CortexUMG/Private/Operations/CortexUMGWidgetPropertyOps.cpp`: the ten setters call `ModifyWidgetForEdit` instead of `WBP->Modify()`.
- `Source/CortexUMG/Private/Tests/CortexUMGTreeIntegrityTest.cpp`: `Cortex.UMG.TreeIntegrity.{ReparentRefusals, AddRefusalsLeaveNoOrphan, DuplicateRefusals, RemoveReleasesNames, UndoRestores}` drive the public router on a compiled transient Widget Blueprint.

## Behavior changes visible to callers

- `duplicate_widget` on the root, or into a full single-child parent, now returns `INVALID_PARENT` instead of a success that orphaned the copy.
- `reparent`/`add_widget` into a full single-child parent return `INVALID_PARENT` instead of a false success.
- `reparent` validates `slot_index` (`INVALID_SLOT_INDEX`).
- `remove_widget` frees the removed names and their variable identities.

## Verification

See [../verification/2026-11-16-issue219.md](../verification/2026-11-16-issue219.md).
