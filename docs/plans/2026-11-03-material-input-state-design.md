# Material root-input state and effective-state diagnostics

Date: 2026-11-03
Status: Approved by user on 2026-11-03; builder-check corrections incorporated; optional further elicitation declined
Issue: https://github.com/etelyatn/UnrealCortex/issues/184
Reporter: etelyatn
Implementation owner: UnrealCortex / CortexMaterial
Delivery endpoint: verified, reviewed, merged delivery; toolkit synchronization included; parent CortexSandbox gitlinks excluded.

## Intent and acceptance

Material graph wires must mean what the Material Editor makes them mean. Callers must be able to inspect and explicitly author serialized root-input constants and masks, access nested reflected material properties, and diagnose authored/effective blend-mode disagreement without Python or manual rewiring.

| Criterion | Current evidence | Route | Minimal proof and exit |
|---|---|---|---|
| Root connections select expressions, not stale inline constants | GraphOps Connect assigns Expression/OutputIndex only; legacy compiler prioritizes UseConstant | Implement | Failing-before/passing-after native regression seeded with UseConstant=true; public routed connect plus actual masked-material rendering with legacy translator recorded |
| Root disconnections match editor constant policy | Disconnect clears Expression/OutputIndex only; engine UpdateInputUseConstant enables constants except disabled inputs | Implement | Native scalar/color/vector/UV representative checks; disabled-inline inputs remain false; constants preserved |
| Read all root-input state, including disconnected inputs | list_connections emits connected edges only | Implement | Exact readback for connected and disconnected scalar/color/vector/UV and nonconstant inputs |
| Write use_constant, constant, output_index and masks | No root-input setter exists | Implement | Typed positive/refusal regressions, unchanged state on invalid requests; routed write/readback |
| Nested reflected property reads/writes | Setter looks up top-level UMaterial only; shared resolver traverses structs/objects | Implement | EditorOnlyData.OpacityMask.UseConstant and nested constants, top-level hidden flag; invalid/null/external-owner paths refuse without mutation |
| Report authored/effective blend modes and opaque override | get_material emits authored blend_mode and is_masked only | Implement | Seed override true; observe mismatch and warning; explicit false write restores effective Masked; clear warning after repair |
| Warn about connected ignored expressions | No root-input diagnostics | Implement | Legacy-translator connected + effective inline constant produces input-targeted warning; disabled-inline properties and new-translator connections do not produce false ignored-expression warnings |
| Optional separate QA/lint operation | Issue explicitly marks optional | No separate operation | Existing material read warnings cover both requested conditions; no new QA subsystem |
| Undo, explicit save and persistence | Existing transaction/batch infrastructure | Preserve/correct affected paths | Undo/redo restores root input and editor data; save + cold reload preserves values, masks, connection and effective blend mode |
| Discovery, MCP and toolkit contracts | Existing material router and generated command discovery | Synchronize | Live schemas accept actual scalar values and typed setter envelope; existing composite still works; focused Python checks and required live benchmark |

Every behavioral row is currently unverified at runtime. No editor was running at intake. Verification exit is the table above plus required build, review, zero-warning project gates and remote integration checks. Add checks only for an unmet row, an observed failure, or a source-confirmed reachable violation of this contract.

## Current source basis

Plugin target/base: c1499002cf4cc53825f55361b39753f9b7413137 (remote main matches).
Toolkit remote main: cc901ae0159f58813f4e48214ff55c86c8cb0bdc. Local toolkit branch contains pre-existing docs commit 2db17d416d118e3c60371d26111e0b3edb48a4e0; preserve it and isolate task publication from it.

- Source/CortexMaterial/Private/Operations/CortexMaterialGraphOps.cpp: root enumeration already uses EMaterialProperty and GetExpressionInputForProperty; names normalize spaces/underscores and CustomizedUV aliases. Connect does not copy source output masks. Disconnect does not update UseConstant.
- Source/CortexMaterial/Private/Operations/CortexMaterialAssetOps.cpp: GetMaterial reports authored blend mode; SetMaterialProperty uses FindPropertyByName directly.
- Source/CortexCore/Private/CortexPropertyUtils.cpp: ResolvePropertyPath supports reflected structs and non-null object traversal, not array-index syntax; returns a leaf property and address, not the owning object.
- UE 5.8 MaterialGraphNode_Root.cpp UpdateInputUseConstant: editor enables inline constants on disconnected supported inputs, but forces false for WorldPositionOffset, Anisotropy, PixelDepthOffset and Displacement.
- UE 5.8 MaterialGraph.cpp LinkMaterialExpressionsFromGraph: calls FExpressionInput::Connect then UpdateInputUseConstant based on whether Expression is null.
- UE 5.8 MaterialShared.cpp CompileWithDefault: UseConstant takes precedence over connected expressions except the four disabled properties.
- UE 5.8 MaterialIRModuleBuilder.cpp instead prioritizes a valid connected expression; MaterialInterface.cpp IsUsingNewHLSLGenerator selects this reachable alternate compiler using material flags and the translator CVar. Editor authoring policy is not a universal claim about compiler precedence.
- UE 5.8 UMaterial::GetBlendMode: bCanMaskedBeAssumedOpaque can turn authored Masked into effective Opaque.

These are observed source facts, not claims of reproduced rendering.

## Approaches

1. **Material-owned extension (selected by user).** Reuse CortexPropertyUtils and existing material reads/router; add only the missing material-specific setter and selected property read. Keeps domain semantics and lifecycle in CortexMaterial.
2. **Shared core nested-property API (not selected).** Generalizes mutation policy to all asset owners. Larger public contract and ownership scope without a demonstrated second consumer.

No new MCP tool, domain, runtime subsystem, shared-core resolver behavior, or cross-domain dependency is needed.

## Public command contracts

All commands remain synchronous, Game Thread routed, using material_cmd(command, params). Existing command names and existing response fields retain their meaning; new fields are additive. No save is implicit.

### material.get_material

Keep asset_path. Add optional include_input_state:boolean=false and properties:array[string] for selected reflected property paths. The properties selector follows existing selected-read conventions: omitted/empty adds no property projection; provided selectors must be non-empty valid strings, all validated before returning a partial result. Return projection under properties keyed by requested path.

Always add:
- effective_blend_mode:string, from GetBlendMode(), using the same complete enum naming helper as authored blend_mode;
- can_masked_be_assumed_opaque:boolean;
- warnings:array[object], each with code and message; root-input warnings also include input.

Retain existing sm6_warnings as its separate established field. Warning codes: BLEND_MODE_MISMATCH and CONNECTED_EXPRESSION_IGNORED. Warnings are response data, not UE warning logs. Emit CONNECTED_EXPRESSION_IGNORED only when a connected constant-capable input has UseConstant=true, the legacy compiler allows its inline constant, and the material is not using the new HLSL generator (query the native selection API). Its message identifies the legacy-translator condition rather than claiming universal precedence across compiler variants. The new translator prioritizes a valid connection, so do not emit that warning there. Do not automatically repair either condition on read or on unrelated property edits.

When include_input_state=true, return root_inputs:array of every available serialized root input, not just active/connected ones. Do not claim unsupported/absent pointers are available. Each record includes:
- input:string (canonical EMaterialProperty-derived name), connected:boolean, source_node:string|null;
- output_index:integer, mask:integer, mask_r/g/b/a:integer (native serialized values);
- supports_constant:boolean (struct capability), inline_constant_allowed:boolean (Material Editor authoring policy, not a compiler-precedence claim);
- use_constant:boolean|null and constant:typed JSON|null. Null means the input type has no inline constant; not a missing/unreadable field.

Constant JSON: scalar number; color object {r,g,b,a}; vector3 object {x,y,z}; vector2 object {x,y}. Values describe native storage without lossy reinterpretation or scalar/color conflation.

### material.list_connections

Keep connections and count. Add optional include_input_state:boolean=false. When true, return the same root_inputs projection as get_material. Existing edge enumeration remains connected-only; do not turn disconnected inputs into edges. Use one native helper for canonical selection and serialization.

Both extended reads use the existing MCP complete-or-refuse response policy. Never truncate root_inputs, selected properties, or connection topology and present the result as complete. An oversized response returns RESPONSE_TOO_LARGE with reader_complete=false and actionable supported guidance: select fewer properties, omit input state, or read root state separately through get_material. Do not advertise unsupported limit/offset pagination. Add focused consumer-visible response-policy coverage for both reads.

### material.set_material_input (new routed command)

Required: asset_path:string, input:string. Optional patch fields: use_constant:boolean, constant:typed JSON, output_index:integer, mask/mask_r/mask_g/mask_b/mask_a:integer.

At least one patch field is required. Omitted fields preserve state. Empty/null/malformed fields are not deletion. Input names use existing normalization and aliases. The selector addresses available serialized root inputs even when the current material domain/blend mode makes them inactive; it must not invent an unavailable input.

Validate the entire patch and prepare all native values before any transaction, PreEditChange or mutation:
- Refuse unknown/absent inputs, unknown fields, wrong types, non-finite/out-of-storage-range numeric values, and invalid output indices.
- Constant/use_constant require a constant-capable input. use_constant=true additionally requires engine inline-constant policy to allow it. Setting false on a capable disabled-inline input is allowed.
- output_index requires a connected expression and a valid output in that expression. Changing the output initializes masks from that output using the native connection operation; explicitly supplied masks then override those defaults.
- Mask writes accept integer 0 or 1; do not silently truncate arbitrary numbers. Reads report native values even if legacy data differs.
- A connected input may explicitly be set use_constant=true for allowed inputs: this is intentional state authoring, not an implicit disconnect. The read emits CONNECTED_EXPRESSION_IGNORED only under the compiler-aware conditions specified above.
- Constant patch updates require complete typed objects for color/vector values; no ambiguous partial color/vector replacement. Scalar is a number. No expression-object pointer editing through this setter.

Response: asset_path:string, updated:true, input_state:object using the read projection, and warnings:array using the same diagnostics. No unrelated input, node, or legacy blend override changes.

### material.set_material_property

Retain asset_path, property_name, value. Extend property_name to dotted reflected paths, including explicit EditorOnlyData.<Property>.<Field>. Other reflected object hops are allowed only while the reached objects belong to this material through their Outer chain; referenced external assets are not writable subobjects of the material. Do not create missing objects or follow null pointers.

Resolve the explicit EditorOnlyData prefix through GetEditorOnlyData(), then reuse ResolvePropertyPath for the remaining struct/object path. Ordinary paths start from the material. Track/validate the reached owner locally to apply correct transaction and edit notifications; do not extend the shared resolver API for this issue.

Keep top-level non-editable UPROPERTY support needed for bCanMaskedBeAssumedOpaque. Preserve existing top-level property authoring, but reject root-input expression-object/whole-input replacements that bypass connection/state validation. Root input leaf paths use the same validation and mutation policy as set_material_input for known fields, so generic paths are not an unsafe back door. Array indexing is not added: use set_material_input for CustomizedUVs0..7; document the existing resolver limitation.

Read selected paths through get_material(properties=[...]); no separate get_material_property command/tool. Unknown/unsupported paths and failed conversion return INVALID_FIELD with the requested path and reason. All failed writes preserve previous values and dirty state; conversion must happen in temporary initialized native storage before committing.

### material.connect / material.disconnect

Resolve and validate before starting the transaction. Use FExpressionInput::Connect for valid outputs so masks match the selected output instead of retaining stale channels. Apply the root input's engine-matching UseConstant policy explicitly: connect clears; disconnect enables for permitted constant-capable inputs, forces false for the engine-disabled four, and leaves nonconstant types without invented fields. Preserve the stored constant.

Root disconnect removes Expression and retains the established OutputIndex=0 behavior. Preserve other state unless native editor semantics require updating it. Accept existing Material/MaterialResult root aliases consistently on these affected paths. Do not add expression-to-expression disconnect support as unrelated scope.

## Native implementation shape

Keep the existing Operations owners. A small private material-input helper may centralize EMaterialProperty/name resolution, typed input metadata, serialization, policy, validation and diagnostics because asset and graph operations need exactly the same rules. No UObject subclass, registry, service, cache or module is introduced. Builder chooses the smallest file/function layout after inspecting existing headers.

Use static type-safe access or reflected metadata verified against the selected input; never cast an arbitrary FExpressionInput to a constant-bearing derived type. Include CustomizedUVs and aliases such as CustomData0 -> ClearCoat and ShadingModel -> ShadingModelFromMaterialExpression. Unsupported engine-version differences belong in the existing CortexEngineCompat mechanism, not ad hoc duplicated version gates.

Transactions span actual mutations and notifications. Modify both UMaterial and the owned editor data when changed. Existing graph transaction scope currently ends before the writes; correct it on the touched connect/disconnect paths. Outside batches, finish with proper property/material notifications. In batches, use existing dirty-material tracking and transaction owner; do not add per-command saves or duplicate recompiles. Undo/redo must restore the editor data as well as the material.

Warnings never mutate the asset. Preserve UObject Game Thread discipline, guarded loading, shared LogCortexMaterial category and existing explicit save/asset lifecycle operations.

## MCP and toolkit synchronization

Expose new parameters/command through native declarations and the existing material router. Confirm scalar value declarations use the repository's actual JSON-any convention rather than declaring every value an object. Synchronize the full capabilities fixture and generated fallback using existing generation scripts, update affected cache/write classification and schema/profile consumers where required, and preserve current composite behavior. Do not create legacy tool wrappers.

Update owning-plugin design and implementation docs and verification report. Update relevant toolkit material guidance/reference on a task-isolated normal branch; preserve the pre-existing toolkit docs commit and do not publish it accidentally. Architecture/system reference changes in the parent workspace require separate scoped documentation commits only if necessary; no parent gitlink updates.

## Verification and delivery

1. TDD native consumer-visible regressions before production behavior: stale constant connection, output masks, disconnect policy, typed round-trips, invalid request preservation, effective-state warnings, nested paths, undo/redo and save/reload. Reuse existing fixture conventions without echo/wiring-only tests.
2. Full supported MSVC CortexSandboxEditor build so fresh module receipts load the candidate; serial Cortex.Material+ runs, focused affected Python suites and required live Cortex validation. Classify expected errors separately and require zero unexpected warnings/errors.
3. Public-route positive smoke on disposable generic assets: record/select the legacy translator on the fixture material, reproduce inline OpacityMask=1, connect black texture channel, authored Masked; observe actual masked rendering after connection. Separately seed opaque override, observe effective-state warning, explicitly clear it, observe corrected effective mode and rendering. Inspect state/masks, disconnect/reconnect and save/cold reload. Verify that selecting the new translator does not produce a false ignored-expression diagnostic. Do not change global translator settings or claim success solely from a wire or transport response.
4. Independent candidate review; fix confirmed findings, refresh only affected evidence. Bind logs/review to candidate/base/dependency SHAs.
5. Scoped commits and normal push, PRs, repository checks, expected-head merge, remote merged revision confirmation and explicit issue reconciliation. Credit etelyatn. Follow safe normal-branch cleanup; no reset/clean/worktrees/force-delete or unrelated editor termination.

Required behavior or verification barriers block integration, not reachable implementation. Exact task ownership, test commands and runtime fixture details belong in the implementation guide after this design is approved.

## Non-goals and builder decisions

No new core property API, standalone QA lint operation, automatic legacy flag repair, array-path grammar, arbitrary external-asset mutation, game logic, expression disconnect expansion, retry/telemetry infrastructure, or parent gitlink publication.

Builder may choose helper layout, fixture names and minimal native test grouping. Any change to public schema, accepted behavior, ownership boundary, save semantics or excluded issue scope requires an explicit design revision.
