# Widget component rename containment

## Requirement

PR #156's `umg.rename_widget` is fingerprint-guarded for one Widget Blueprint. It must refuse before mutation when Unreal's rename would rewrite a loaded external Blueprint, including references to generated UI-component members rather than the bare designer widget member.

## Root cause

UE 5.8 `FWidgetBlueprintOperationUtils::RenameWidget` calls `FUIComponentUtils::ReplaceComponentVariableReferences` after structural refresh. The component extension derives compound member names with `UUIComponentContainer::GetPropertyNameForComponent` and invokes Blueprint member-reference replacement. The existing Cortex preflight checked only the bare widget name.

## Correction

Reuse the existing dependent-Blueprint scan. Include each attached component's native generated member name in the same reference check before starting the transaction. Read the existing extension only; do not create an extension during preflight. Engines without the component extension retain the existing bare-widget path through a header-availability guard. Do not broaden refusal to dependencies that merely store the target widget's class.

No command parameters, response fields, module dependencies, persistence format, or cross-domain ownership change. Rename remains target-only, transactional, and explicitly unsaved. This is a guard correction, not external-asset rename support.

## Acceptance

A compiled component-bearing widget with a real external generated-member getter must refuse with `INVALID_OPERATION`, preserving widget/component target, getter identity/member name, and both package dirty states. Existing bare-member refusal, class-instance-only success, slot-first animation binding repair, undo/redo, and case-sensitive rename behavior must remain intact.

## Broad-suite fixture lifetime correction

Rendered acceptance additionally exposed a discarded remove-graph recovery fixture remaining eligible for PIE's dirty-Blueprint compilation. The fixture intentionally stages synthetic FieldNotify metadata and is not a runnable asset. Its shared test cleanup must discard the Blueprint itself, not only its package: clear public/standalone flags, mark the Blueprint garbage, then retain existing package cleanup. This is test-owned lifetime correction only; do not change production compilation, GC processor selection, recovery contracts, or synthetic journal coverage.

Acceptance requires `IsValid(BP)==false` immediately after cleanup and a fresh broad suite. The separately observed earlier GC access violation is historical evidence, not a proven consequence of this PIE compilation failure.

## Obsolete broad-suite assertions

Remove the old Data batch subcase claiming zero-command success: the authoritative Core router rejects zero-command batches, and `Cortex.Core.Batch.EmptyBatch` already covers that refusal. Do not change production behavior or duplicate the canonical contract test.

Remove `EmptyProviderSilentDrop`, which asserts Markdown heading wording and assumes that the ambient `thisAsset` provider is empty. A rendered broad suite retains editor context, so that premise is not isolated. Do not re-pin formatting, force the provider empty, or change production context resolution to satisfy the incidental assertion.
