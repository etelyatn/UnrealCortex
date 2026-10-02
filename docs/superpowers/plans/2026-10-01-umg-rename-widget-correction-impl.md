# UMG Widget Rename Animation Binding Correction Implementation Plan

> **Goal:** Ensure `umg.rename_widget` updates all animation bindings and possessables for a renamed widget, resolving the engine defect where encountering a slot binding halts processing of later same-widget bindings, while maintaining full transactional undo/redo atomicity and strict case sensitivity.

**Spec:** `Plugins/UnrealCortex/docs/superpowers/specs/2026-10-01-umg-rename-widget-correction-design.md`  
**Date:** 2026-10-01  
**Status:** Regression verified; overall acceptance pending

---

## Task 1: Deterministic Forward Regression Test (RED)

- **Test Class:** `FCortexUMGRenameWidgetSlotFirstAnimationBindingTest`
- **Current Namespace:** `Cortex.UMG.RenameWidgetSlotFirstAnimationBinding`
- **Historical Namespace:** `Cortex.UMG.RenameWidget.SlotFirstAnimationBinding` (historical RED/GREEN runs were executed under this namespace; sibling naming was subsequently adopted to avoid Unreal's hierarchical test tree masking existing leaf test `Cortex.UMG.RenameWidget`)
- **File:** `Plugins/UnrealCortex/Source/CortexUMG/Private/Tests/CortexUMGWidgetVariableTest.cpp`
- **Procedure:**
  1. Construct `FCortexUMGAnimationBindingFixture` containing `BodySizeBox` and initial direct animation binding.
  2. Insert an authored slot binding for `BodySizeBox.Slot` (`LayoutData.Offsets.Left`) at index 0, placing it before the direct binding at index 1.
  3. Snapshot track identities and author two keyed float values (frame 120 / value 10.0, frame 720 / value 50.0).
  4. Perform `umg.rename_widget` to `RenamedBodySizeBox`.
  5. Assert both slot and direct bindings update their target names, resolve their respective runtime objects, and preserve GUIDs, slot names, tracks, section ranges, and keys.
- **Observed Forward RED Evidence (RIP48-rename-slot-first-red-valid-20261001.log):**
  - Line 508: direct `WidgetName` remained `BodySizeBox` instead of `RenamedBodySizeBox`.
  - Line 515: direct runtime object resolved to `nullptr`.
  - Line 516: direct runtime object mismatch against renamed widget.
  - Line 524: direct possessable name remained `BodySizeBox`.

---

## Task 1.5: Transactional Undo/Redo Regression Test (RED)

- **Test Extension:** Reset transactor to clean setup boundary (`GEditor->ResetTransaction`) prior to rename, followed by post-forward `GEditor->UndoTransaction()` and `GEditor->RedoTransaction()` assertions.
- **Observed Undo RED Evidence (RIP48-rename-animation-undo-red-20261001.log):**
  - Completed under historical namespace with QueueEmpty / exit 0 and exactly six expected assertion failures:
    - Line 598: `slot binding target widget name restored after undo` (remained `RenamedBodySizeBox`).
    - Line 601: `direct binding target widget name restored after undo` (remained `RenamedBodySizeBox`).
    - Line 603: `slot binding resolves runtime object after undo` (`nullptr`).
    - Line 604: `slot binding resolves to restored widget slot after undo` (mismatch).
    - Line 609: `direct binding resolves runtime object after undo` (`nullptr`).
    - Line 610: `direct binding resolves to restored widget after undo` (mismatch).
  - Forward rename: 0 errors; possessable undo/redo: 0 errors (MovieScene snapshot already captured original).

---

## Task 1.6: Case-Sensitivity Regression Test (RED Verified)

- **Test:** `FCortexUMGRenameWidgetTest` (in `Plugins/UnrealCortex/Source/CortexUMG/Private/Tests/CortexUMGWidgetVariableTest.cpp`)
- **Added Assertions:**
  1. Wrong-case `widget_name` (`TEXT("bodysizebox")` vs `TEXT("BodySizeBox")` with `new_name: "BodySizeBox"` to isolate lookup and prevent mutation cascade): asserts refusal with `WidgetNotFound` (`bSuccess == false`).
  2. Case-only `new_name` (`TEXT("bodysizebox")` when widget is `TEXT("BodySizeBox")`): asserts refusal with `InvalidOperation` (`bSuccess == false`).
  3. Preservation of object identity, property binding reference, animation reference, and package dirty state across both refusals.
- **Observed Case-Sensitivity RED Evidence (RIP48-rename-case-sensitive-red-20261001.log):**
  - `originalRenameWidget` failed with exactly four expected assertion failures:
    - Wrong-case refusal (`TestFalse` failed; operator!= returned false).
    - Expected `WidgetNotFound` (actual empty error code due to premature success).
    - Case-only refusal (`TestFalse` failed; operator!= returned false).
    - Expected `InvalidOperation` (actual empty error code due to premature success).
  - Sibling slot-first test (`Cortex.UMG.RenameWidgetSlotFirstAnimationBinding`) was unaffected and succeeded (2 complete queue / status 0, 0 warnings/fatals).

---

## Task 2: Production Correction (Outer Transaction, Pre-Snapshot, Case Sensitivity)

- **File:** `Plugins/UnrealCortex/Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp`
- **Implemented Changes:**
  1. Enclose the mutation block in an outer `FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Cortex: Rename Widget %s to %s"), *OldName, *NewName)))`.
  2. Prior to calling `FWidgetBlueprintOperationUtils::RenameWidget`, iterate through `WBP->Animations` and call `WidgetAnimation->Modify()` and `WidgetAnimation->MovieScene->Modify()` for all animations containing bindings referencing `OldFName`.
  3. Execute `FWidgetBlueprintOperationUtils::RenameWidget(WBP, Widget, NewName)`. On false-return, call `Transaction.Cancel()` to avoid an empty undo history entry, and return `InvalidOperation`.
  4. In the post-repair loop, update any remaining bindings matching `OldFName` to `NewFName` and rename possessables for direct bindings (`SlotWidgetName == NAME_None`). No repetitive `Modify()` calls needed.
  5. Maintain full preservation of slot names, GUIDs, tracks, section ranges, and keys across both forward apply and undo/redo.
  6. Applied case-sensitive string comparisons at lines 841 and 844: `if (!Widget || !Widget->GetName().Equals(WidgetName, ESearchCase::CaseSensitive)) return WidgetNotFound;` and `const bool bChanged = !OldName.Equals(NewName, ESearchCase::CaseSensitive);`, while retaining the existing `Widget->GetFName() == FName(*NewName)` check to refuse case-only renames with `InvalidOperation`.

---

## Task 3: Contract Documentation

- **File:** `Plugins/UnrealCortex/MCP/README.md`
- **Changes:**
  - Added `### UMG widget rename contract` documenting identifier discipline, `expected_fingerprint` preconditions (mismatch returns `StalePrecondition`), containment safeguards against loaded children and external member references, binding preservation including slot-first ordering, and structural refresh semantics.

---

## Task 4: Verification Status & Boundaries

- **Forward RED Proof:** Confirmed via coordinator execution with exact four expected assertion failures.
- **Undo RED Proof:** Confirmed via coordinator execution (`RIP48-rename-animation-undo-red-20261001.log`) with exact six expected assertion failures under historical test namespace.
- **GREEN Proof:** Confirmed via coordinator execution (`RIP48-rename-animation-green-20261001.log`): `SlotFirstAnimationBinding` 1/1 Success, QueueEmpty / status 0, Warnings/Errors/Fatals 0/0/0 under historical test namespace. All forward rename, runtime object resolution, authored key/track/range, and Undo/Redo assertions passed.
- **Sibling Dual-Registration Proof:** Confirmed via coordinator execution (`RIP48-rename-sibling-registration-green-20261001.log`): `RunTests Cortex.UMG.RenameWidget` executed both `Cortex.UMG.RenameWidget` and `Cortex.UMG.RenameWidgetSlotFirstAnimationBinding` with 2/2 Success, QueueEmpty 2 / status 0, Warnings/Errors/Fatals 0/0/0.
- **Case-Sensitivity RED Proof:** Confirmed via coordinator execution (`RIP48-rename-case-sensitive-red-20261001.log`): `originalRenameWidget` failed with 4 exact assertion failures; sibling slot-first test unaffected and GREEN.
- **Case-Sensitivity GREEN Proof:** Confirmed via coordinator execution (`RIP48-rename-case-sensitive-green-20261001.log`): both original `Cortex.UMG.RenameWidget` and sibling `Cortex.UMG.RenameWidgetSlotFirstAnimationBinding` executed with 2/2 Success, QueueEmpty 2 / status 0, Warnings/Errors/Fatals 0/0/0.
- **Live MCP Smoke Verification:** Confirmed via coordinator execution (`Saved/RIP48Validation/live-case-corrected-20261001.json`, Editor PID 38904, bound to Source SHA256 `1fbac25cac8de59d102ed2cb6d5cc5d31cf7217e04034dbfee5528287ed0c541`):
  1. Wrong-case current widget name lookup refused with `WIDGET_NOT_FOUND`.
  2. Case-only new name refused with `INVALID_OPERATION`.
  3. Existing collision, bad identifier, and stale fingerprint guards refused while preserving clean fingerprint, saved hash, and widget tree intact.
  4. Exact same-name no-op rename returned unchanged.
  5. Live registered FastMCP `umg_cmd` rename executed successfully (`changed=true`, `saved=false`).
  6. Blueprint compile (0 errors, 0 warnings), package save, `core.reload_asset`, and readback clean with new name and variable flag intact; test asset deleted cleanly.
- **Verification Boundary:** Regression verified; overall acceptance pending. Full Graph suite acceptance is independently blocked by a reproduced GC crash; this rename correction does not establish its cause or resolution.

## Final PR #156 Acceptance (2026-10-03)

The historical verification boundary above is superseded by the final candidate acceptance in [the PR #156 report](../../verification/2026-10-02-pr-156.md). The normal full native run passed 1,649/1,649 tests. This is observed acceptance, not a claim that the earlier intermittent GC crashes have an established root cause. The final candidate also includes generated UI-component member-reference containment.
