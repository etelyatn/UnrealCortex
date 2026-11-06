# UMG serialized property-binding inspection and authoring

## Request and authority

Issue: https://github.com/etelyatn/UnrealCortex/issues/167 (reporter: etelyatn).
User authorizes end-to-end implementation, verification, PR, policy-compliant merge, and issue reconciliation. The discussion expands removal into a fully implemented nullable setter and explicitly prefers extending existing inspection surfaces. Design direction approved; this written specification requires review before implementation planning.

Base: UnrealCortex main c05e89bd8aaae5b21af7078ab1a4dfbbeeb8490e. Implementation owner: CortexUMG in UnrealCortex. Existing MCP tool: umg_cmd. No new standalone MCP tool, composite, list command, or separate removal command.

## Outcome

Inspect serialized Designer property bindings and create, replace, or clear exactly one binding while preserving the authored widgets, hierarchy, styles/defaults, animations, and other bindings. Explicit null clears; a non-null binding object authors a validated source. Compilation, saving, and reload remain separate existing operations.

Non-goals: project-specific migration, orphan cleanup, bulk replacement, widget recreation, graph generation, automatic source-variable deletion, MVVM/extension binding authoring, runtime delegate injection, dependent-package persistence, or unsafe private-memory access.

## Current evidence and reuse

- CortexUMG GetWidget returns widget metadata; GetTree returns hierarchy. Neither reads UWidgetBlueprint::Bindings.
- GetProperty reads literal widget property values through ResolvePropertyPath, not editor bindings. SetProperty must retain literal-value semantics.
- Animation binding commands operate on UWidgetAnimation::AnimationBindings and MovieScene data; they are not property-binding readers.
- UE 5.8 WidgetBlueprint.h publicly exposes the editor-only Bindings array and FDelegateEditorBinding fields. SourcePath contains ordered segments with serialized Struct, MemberName, MemberGuid, and IsProperty fields.
- GetMemberName can return NAME_None when a saved GUID no longer resolves. Therefore getters alone cannot prove complete serialized source identity.
- Designer removal uses Bindings.Remove with equality comparing only ObjectName and PropertyName. Cortex must count matches and reject duplicates rather than remove every equal target.

## Read contract: reuse get_widget and get_tree

Both commands gain optional boolean include_property_bindings (default false). Existing response behavior stays unchanged when absent/false. Invalid parameter types refuse.

When true, add a property_binding_state object:

- reader_complete: true only when every serialized field was read successfully; reader failure returns an explicit error, never an empty success.
- scope: asset for get_tree; widget for get_widget.
- total: total records in the requested scope.
- bindings: ordered serialized records with widget_name, property_name, kind, function_name, source_property, member_guid, and source_path.
- source_path: ordered segments containing serialized owner path, member name, member GUID, and property/function discriminator. Resolution status is diagnostic metadata, not a substitute for serialized identity.
- fingerprint: package fingerprint plus versioned deterministic domain signature over the complete asset-level binding array, preserving record order and every serialized source identity field. Both reads return the same asset guard, even when get_widget filters records.
- diagnostics: unresolved sources/targets remain visible. get_tree includes records targeting missing widgets, including bindings when the Designer tree is empty.

Do not paginate initially. No artificial native binding count limit. Oversized MCP inspection returns an explicit RESPONSE_TOO_LARGE refusal preserving fingerprint, counts, and reader_complete=false; do not claim a complete empty list. Widget-scoped retry is suggested only for existing widget targets; oversized orphan inspection has an honest terminal size refusal, not an impossible retry promise. Reject presence of limit, offset, or cursor on binding writes and opted-in binding reads (including explicit null) before generic MCP pagination interception.

Use one internal reader for both reads, mutation preflight, fingerprinting, and readback. Public binding fields are read directly. For private reflected path-segment fields, use typed native Unreal property metadata with exact field/type validation, read-only. No offset assumptions, access casts, Python protection bypass, or generated-class-only approximation. Missing expected metadata fails closed. Serialized field names and types must be checked against supported engine contracts before claiming compatibility.

## Write contract: set_property_binding

Required params: asset_path, widget_name, property_name, binding (object or explicit null), expected_fingerprint (object).

- Omitted binding, empty object, strings, arrays, and malformed fields return INVALID_FIELD before mutation.
- binding=null clears an existing exact target. A valid widget with no record returns changed=false and serialized absence. Missing widget returns WIDGET_NOT_FOUND; do not silently clear arbitrary orphan targets.
- binding object supports kind=property with an ordered source_path of member names, or kind=function with function_name. Sources are self-context members of the Widget Blueprint and nested reflected property chains. Do not accept raw serialized struct addresses or arbitrary owner-object mutation.
- Creation validates an attribute property and its PropertyNameDelegate, never an event-delegate fallback. Walk source names from self through reflected object/struct property types; reject unsupported intermediate shapes rather than flattening names against self. Use native terminal compatibility validation and explicitly require pure/const functions. Refuse non-null authoring when ArePropertyBindingsAllowed() is false; null clearing remains permitted. Construct a fresh FDelegateEditorBinding for replacement, initialize all kind-dependent fields, and leave no legacy SourceProperty on function records. Resolve an existing function graph GUID when available; no graph creation.
- Zero matching records creates; one replaces; more than one returns PROPERTY_BINDING_AMBIGUOUS without mutation. No occurrence/index override to bypass ambiguity.
- expected_fingerprint is mandatory. Missing/malformed guard refuses; mismatches return STALE_PRECONDITION and current fingerprint. Unsaved changes to any binding invalidate an earlier guard. Run mutation-block guards before side effects.
- Identical canonical binding is a no-op (changed=false). Equality must cover every serialized field, not FDelegateEditorBinding::operator==.

Response: asset_path, widget_name, property_name, changed, before_binding (object|null), binding (serialized post-state object|null), reader_complete, fingerprint, compiled=false, saved=false. Outcomes are actual serialized array readback, not predictions. An error never reports successful mutation. Removal must verify the target is absent and all retained records are identical; replacement verifies the authored record and retained order/identity.

## Mutation and lifecycle

All UObject reads/writes occur on Game Thread through existing command routing. Validate all input, source/target compatibility, duplicates, reader availability, and fingerprint before opening the transaction or marking dirty.

Use one FScopedTransaction and WBP->Modify(). Change only the selected Bindings entry (or append a new record), then verify serialized post-state before modification notification. Preserve unrelated order. No widget defaults are cleared; Designer extension callbacks are outside this serialized contract. A readback failure restores the original binding array and dirty state, cancels the transaction, and reports failure; failed restoration uses existing mutation-block policy rather than pretending success. After successful readback, call MarkBlueprintAsModified (not MarkBlueprintAsStructurallyModified, which compiles the skeleton). Notification is the success boundary; perform no fallible identity validation afterward. Capture verified post-state for response and refresh its package fingerprint after notification.

Explicit blueprint.compile and core asset save operations remain responsible for lifecycle. Tests must establish that the supported binding mutations survive explicit compilation and save/reload without relying on compiler sanitization to remove a stale source. No implicit dependent saves/reloads.

## Acceptance ledger

| Criterion | Route | Required proof |
|---|---|---|
| ProgressDisplay.Percent variable binding cleared | Implement setter null | Serialized native before/after and actual routed MCP readback |
| Widget and unrelated bindings unchanged | Preserve exact record/objects | Native identity, hierarchy, defaults/styles and animation snapshots |
| No stale reference to removed source | Direct serialized mutation | Inspect before source removal and after explicit compile/save/reload |
| Missing target | Non-destructive refusal/no-op as specified | Missing widget error and unbound valid widget no-op |
| Ambiguous target | Non-destructive refusal | Duplicate serialized target fixtures |
| Mismatched guard | Non-destructive refusal | Clean-to-dirty and dirty-to-dirty stale guard cases |
| Reader unavailable/incomplete | Fail closed | Typed reader validation and transport truncation behavior |
| Persisted requested state | Explicit lifecycle | Native save/reload plus live MCP explicit compile/save/fresh-load readback |
| Create property-path binding | Expanded user scope | Compatible nested property source and incompatible source refusal |
| Create function binding | Expanded user scope | Existing compatible function and invalid function refusal |
| Replace/idempotent set | Expanded user scope | Exact replacement, unchanged retained records, no-op semantics |
| Null versus omitted/empty | Safe API | Malformed requests do not mutate or dirty |
| Undo/redo | Editor authoring correctness | Transaction restores exact array on undo and reapplies on redo |
| Existing inspection reuse | API scope | Opt-in get_widget/get_tree and no new MCP tool |

## Verification and delivery

TDD red/green native behavioral tests, supported MSVC build, Cortex.UMG+ regression suite with zero raw warnings, affected Python contract/response tests, live actual MCP command/schema discovery and positive authoring/clear smoke, explicit compile/save/reload, independent candidate review. Serialize native runner and task-owned Editor usage. Preserve unrelated Editors and workspace changes.

Synchronize affected toolkit documentation/capabilities through cortex-sync-toolkit; publish only required scoped changes with explicit cross-repository reconciliation. Update system references, implementation guide, and candidate-bound verification report. Do not close #167 before all original and expanded acceptance criteria are verified and the candidate is confirmed merged.

## Simplicity check

- [x] Think before acting — issue, native owner, existing reads, and user-selected inspect/set direction inspected.
- [x] Simplicity first — existing umg_cmd and opt-in reads; one coherent domain setter, no new tool/list/remove surface.
- [x] Surgical changes — CortexUMG binding owner plus required contracts/docs; no literal property behavior changes or unrelated cleanup.
- [x] Goal-driven execution — native and actual MCP authoring/clear/persistence acceptance exercised; evidence and remaining review/publication gates are recorded in docs/verification/2026-10-06-issue167-property-bindings.md.

## Elicitation Findings

### Failure Mode Analysis (2026-10-06)

**Engine Architect findings:** Native modification notifications alter compile status/caches; verify before notification. Native path validation does not establish adjacent-owner connectivity or attribute function purity; walk reflected types and enforce purity. Honor ArePropertyBindingsAllowed for authoring while retaining null repair. Fresh replacement records prevent legacy SourceProperty from changing a function binding during compile.

**AI Coding Expert findings:** Nested binding arrays require an explicit size-refusal envelope preserving fingerprint and false completeness; get_widget cannot recover orphan-target records. Binding operations must reject pagination fields before generic MCP cursor interception.

**Cross-domain risks:** Immediate serialized success is not lifecycle proof: compile can reinterpret stale kind-dependent fields. Generic response/cursor handling can falsely represent inspection or mutation without native execution.

**Resolution:** Accepted by user. Remedies incorporated into read, write, and transaction contracts. Add regression coverage for legacy property-to-function replacement, impure/incompatible sources, disabled binding policy, oversized orphan read, and cursor-bearing mutation.

## Compiler compatibility correction (PR171)

JSON request-field validation binds entries with `const auto&` and converts keys through the existing `CortexEngineCompat::JsonKeyToString` helper. UE5.8 uses shared-string JSON keys; explicitly binding an FString-keyed pair constructs a temporary and fails Clang's range-loop check. Pre-5.8 conversion remains owned by the existing helper. Allowed fields, error text, mutation guards and public contracts are unchanged.

Verification: [PR171 compiler and binding evidence](../verification/2026-10-08-pr171.md). No new toolkit or system architecture changes.
