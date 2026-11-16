# Issue 219: UMG tree mutations must keep the hierarchy intact and undoable

Request: https://github.com/etelyatn/UnrealCortex/issues/219, reported by @XpycT (found by code reading at `f72a0ad`).

## Outcome and boundary

`umg.add_widget`, `umg.remove_widget`, `umg.reparent`, `umg.duplicate_widget` and the `umg.set_*` property setters must never report success for a change that did not happen, never corrupt the hierarchy (cycles, lost or orphaned widgets), never leave a name occupied by a widget that is no longer in the tree, and must journal every object they write so that editor undo restores the change. Command names, parameters and success response shapes stay the same. No new command.

## Source basis

At base `bc7c94bf` (0.4.7), `Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp`:

- `Reparent` removed the widget from its old parent and then called `AddChild`/`InsertChildAt` with no checks in between. Moving the root or a widget under itself or a descendant produced a cycle. A full single-child target (Button, Border, SizeBox, ScaleBox) returned a null slot after the widget had already been detached, so the widget was lost while the response said `reparented: true`. `slot_index` was not validated.
- `AddWidget` constructed the widget before validating the parent, slot index and root slot, and ignored the result of `AddChild`. A refusal or a full single-child parent left a detached widget outered to the `WidgetTree` that still held its name. The full-parent case returned `added: true` and registered a variable.
- `DuplicateWidget` ignored a null `AddChild` (full parent), "succeeded" for the root (no parent, so the copy was orphaned), and checked only the new root name. An occupied prefixed child name made `ConstructWidget` replace the existing widget object in place.
- `RemoveWidget` only unlinked the widget. The subtree kept its outer and names, and `WidgetNameExists` walked the attached tree only, so the name looked free. Re-adding that name with another class trips the engine's same-name/different-class check in `StaticAllocateObject`.
- `RemoveWidget`, `Reparent`, `DuplicateWidget` and the ten setters in `CortexUMGWidgetPropertyOps.cpp` called only `WBP->Modify()`. The panels, slots and widgets they wrote were not journaled.

Engine reference: `FWidgetBlueprintEditorUtils::DeleteWidgets` (UE 5.8) journals the tree, the asset, the parent and every removed widget, removes through `UWidgetTree::RemoveWidget`, renames the removed subtree into the transient package and calls `OnVariableRemoved`. `UPanelWidget::AddChild` checks capacity before detaching the content from its current parent.

## Selected design

- **Reparent:** refuse with `INVALID_PARENT` when the new parent is the widget or one of its descendants (walk `GetParent()` from the new parent; this also covers the root), or when `CanAddMoreChildren()` is false. Validate `slot_index` against `[0, count]`, or `[0, count - 1]` for a move within the same parent (`INVALID_SLOT_INDEX`). Do not detach explicitly: `AddChild` detaches only after its own capacity check. Journal the asset, widget, old slot, old parent and new parent. `slot_index` in the response is the actual index.
- **Add:** validate the root slot, parent existence, panel type, capacity and slot range before `ConstructWidget`. Journal the asset, tree and parent.
- **Duplicate:** refuse a parentless source and a full parent (`INVALID_PARENT`). Check every planned name (new root plus each prefixed descendant name) for an existing widget or a duplicate within the plan (`WIDGET_NAME_EXISTS`) before constructing anything. Journal the asset, tree and parent.
- **Remove:** mirror `DeleteWidgets` in place: journal the tree, asset, parent, slot and subtree, call `UWidgetTree::RemoveWidget`, then rename each removed widget into the transient package and call `OnVariableRemoved`. Graph variable nodes and delegate bindings stay untouched, as before. The engine variant that silently deletes graph nodes was not selected (maintainer decision).
- **Name check:** new `CortexUMGUtils::WidgetNameHeldByOtherClass` refuses (`WIDGET_NAME_EXISTS`) a name held by a detached object of another class outered to the tree, such as a widget saved by an older remove, because constructing on it aborts. A same-class occupant is still reused in place, as before: an undone `add_widget`/`duplicate_widget` keeps its object for redo, and the name must stay reusable (review finding on the first candidate, which refused every outered name).
- **Setters:** `CortexUMGUtils::ModifyWidgetForEdit` journals the asset, the widget and its slot. All ten setters use it.

## Acceptance

- Reparent of root under a descendant, of a widget under itself or under a descendant, into a full Button, or with an out-of-range `slot_index` is refused and leaves the hierarchy unchanged. A valid `slot_index` places the widget there.
- `add_widget` refusals (full Button, missing parent, non-panel parent, bad slot, second root) leave no object in the tree outer and no variable.
- `duplicate_widget` into a full parent, of the root, or with a colliding child name is refused with no object created and the existing widget untouched. A valid duplicate still succeeds.
- After `remove_widget`, the subtree is in the transient package, its names and variable identities are released, and a different class can reuse the name. A detached object of another class holding a name refuses `add_widget` on that name. After an undone `add_widget`, the same widget can be added again.
- Undo restores remove (membership, names, outers), reparent, duplicate, `set_text` (widget) and `set_padding` (slot).
