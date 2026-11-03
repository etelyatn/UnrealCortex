# Material Input State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver UnrealCortex issue #184 with editor-equivalent root rewiring, typed root-state authoring/inspection, nested material property access and truthful effective-state diagnostics.

**Architecture:** CortexMaterial owns the behavior and uses CortexPropertyUtils for reflected traversal. Existing material_cmd and reads carry new contracts; one new routed set_material_input command supplies missing authoring. Existing complete-or-refuse MCP policy protects read completeness.

**Tech Stack:** Unreal Engine 5.8 / supported plugin UE 5.6+, C++, Unreal automation, Python/uv/pytest, existing MCP router, Cortex Toolkit Markdown.

**Spec:** [Approved design](2026-11-03-material-input-state-design.md). Read the full design, particularly exact request/response fields, before execution.

## Global Constraints

- User endpoint: merged delivery, including toolkit synchronization; parent CortexSandbox gitlinks excluded.
- Plugin base c1499002cf4cc53825f55361b39753f9b7413137; task branch fix/material-input-state-184.
- Toolkit base cc901ae0159f58813f4e48214ff55c86c8cb0bdc. Preserve local docs/pr46-migration-contract at 2db17d416d118e3c60371d26111e0b3edb48a4e0; create a separate normal task branch from toolkit origin/main. Never publish the pre-existing ahead commit accidentally.
- All shell commands through rtk. No worktrees, reset/clean/stash, force pushes/deletion, user-file cleanup, or unrelated editor termination.
- No new domain, MCP tool, shared-core resolver behavior, or cross-domain dependency. All UObject access on Game Thread. Writes transaction-scoped and explicitly saved only when requested through existing save operations.
- Red before production behavior. One serial full-target build/test at a time. Never trust tests after a failed build or missing fresh module receipts.
- Diagnostics are response data, not UE warnings. Zero unexpected warnings/errors; separate expected error declarations from raw diagnostics.
- Existing top-level material property authoring remains supported. New nested paths stay inside the material's owned UObject graph; no external-object mutation or whole-root-input replacement.
- Constant JSON: scalar number; color {r,g,b,a}; vector3 {x,y,z}; vector2 {x,y}. Omission preserves; invalid or explicit null is not clearing. Native read values are not silently normalized.

## Review Focus

1. A texture R/G/B/A output must copy the selected output's mask, not merely store its index; Task 1 pins channel changes and reconnect behavior.
2. Engine-disabled inline inputs and nonconstant inputs must not be treated as scalar inputs; Task 1 pins WorldPositionOffset/Anisotropy/PixelDepthOffset/Displacement and nonconstant records.
3. A generic root leaf path must not bypass typed validation or mutate external assets; Task 2 pins leaf/member writes, expression replacement refusal and failed-conversion preservation.
4. Diagnostics must not claim the new translator ignores a connected expression; Task 1 pins compiler selection and Task 5 records the legacy rendering path.
5. Oversized selected properties or topology must never hide missing root records behind truncation; Task 3 pins complete-or-refuse at the actual response formatter.

## Ownership and execution

User execution update (2026-11-03): implement in subagents. NativeGraphImpl owns InputUtils, GraphOps and InputStateTest; NativeAssetImpl owns AssetOps and AssetTest. These workers coordinate the fixed helper contract and never share edits. Python response work and toolkit guidance use separate subagents. Main agent owns shared interface decisions, serialized Red/Green builds/tests, rendering/persistence acceptance, independent review, integration and publication. Workers stage tests first, freeze at controller verification barriers, and implement behavior only after observed RED. No worker publishes or runs overlapping checks.

Fresh independent reviewers inspect the integrated candidate; their reports do not substitute for controller acceptance. No open-ended audit after all named criteria and gates pass.

## File responsibilities and interfaces

Plugin-relative paths unless otherwise stated:

- Existing Operations/CortexMaterialGraphOps.cpp/.h: connect/disconnect, list_connections and new static FCortexCommandResult SetMaterialInput(const TSharedPtr<FJsonObject>& Params).
- Existing Operations/CortexMaterialAssetOps.cpp: get_material diagnostics/selected reads and nested set_material_property.
- New Source/CortexMaterial/Private/Operations/CortexMaterialInputUtils.h/.cpp: shared canonical root selector, typed state projection, engine constant policy and warnings. No service/cache/UObject type.
- Existing Source/CortexMaterial/Private/CortexMaterialCommandHandler.cpp: dispatch and live declarations.
- New Source/CortexMaterial/Private/Tests/CortexMaterialInputStateTest.cpp: behavior regressions and lifecycle fixtures; existing graph/asset tests remain unless their affected assertion is incidental/wiring-only.
- MCP/src/cortex_mcp/response.py and MCP/tests/test_response.py: complete-or-refuse policy and consumer-visible boundary tests.
- MCP/tests/fixtures/capabilities_cache_full.json and MCP/src/cortex_mcp/_fallback_generated.py: synchronized discovery, generated only after native declaration stabilization.
- Existing toolkit resources/mcp-tool-reference.md and skills/cortex-material/SKILL.md: truthful commands, examples and limits; no new skill.
- This design/implementation pair and plugin docs/verification/2026-11-03-issue184-material-input-state.md: decisions, task state, bound evidence and delivery.

Shared helper interface (namespace CortexMaterialInputUtils):

```cpp
bool ResolveInput(UMaterial* Material, const FString& Name,
    EMaterialProperty& OutProperty, FExpressionInput*& OutInput);
TSharedRef<FJsonObject> SerializeInput(UMaterial* Material, EMaterialProperty Property);
TArray<TSharedPtr<FJsonValue>> SerializeInputs(UMaterial* Material);
TArray<TSharedPtr<FJsonValue>> CollectWarnings(UMaterial* Material);
void UpdateUseConstant(UMaterial* Material, EMaterialProperty Property, bool bDisconnected);
```

Helpers are private to CortexMaterial; typed patch staging may remain private in its owner. ResolveInput shares existing normalized names and aliases and rejects absent inputs. SerializeInput uses the approved fields and explicit null for nonconstant data. UpdateUseConstant matches Material Editor authoring policy, independent of diagnostic compiler precedence. Do not expose unneeded helper APIs beyond these consumers.

Native parameter Type is descriptive FString metadata, forwarded unchanged by operation schemas; declared-contract enforcement does not type-check values. Use descriptive `any` for set_material_property.value and set_material_input.constant, with concrete allowed forms in descriptions and actual handler validation. Keep masks/output indices declared number with integer constraints documented. No CortexCore typing change.

## Commands for controlled evidence

Run from CortexSandbox root. Prefer native Windows Git Bash, not WSL Bash, for existing Windows scripts:

```text
rtk proxy "C:/Program Files/Git/bin/bash.exe" -lc 'ENGINE_PATH=$(python cortex-toolkit/lib/cortex_config.py --project-dir . --get engine.path | tr -d "\r"); export DOTNET_ROOT="$ENGINE_PATH/Engine/Binaries/ThirdParty/DotNet/10.0/win-x64" DOTNET_MULTILEVEL_LOOKUP=0 DOTNET_ROLL_FORWARD=LatestMajor; rtk proxy "$ENGINE_PATH/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" CortexSandboxEditor Win64 Development -Project="$(pwd)/CortexSandbox.uproject" -WaitMutex -FromMsBuild'
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.Material.InputState+' }"
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.Material+' }"
```

Record actual engine path, candidate SHA/source state and output artifact paths. If a call backgrounds, its returned handle is not build completion: wait for the automatic final result before the next dependent command. Read the relevant test skills before running checks.

### Task 1: Native root state and connection semantics

**Files:** new InputUtils pair/test above; GraphOps.cpp/.h; AssetOps.cpp get_material; CommandHandler.cpp.
**Consumes:** existing loading, serializer, batch scope, root enumeration and expression APIs.
**Produces:** helper interface above; new set_material_input dispatch; include_input_state read projections; diagnostics/effective blend fields; corrected connect/disconnect.

- [ ] Write native tests under Cortex.Material.InputState.* before behavior. Seed OpacityMask.UseConstant=true, Constant=1, connect a known expression, assert Expression identity, UseConstant=false and unchanged Constant. Connect distinct texture channel outputs and assert OutputIndex plus exact five mask fields. Disconnect and assert constant preservation and editor-equivalent UseConstant.
- [ ] Add representative scalar/color/vector/UV setter/read tests with exact native values; include CustomData0/ClearCoat mapping and all four engine-disabled properties. Nonconstant inputs expose supports_constant=false and null constant/use_constant; attempted constant writes refuse.
- [ ] Add invalid-patch state snapshots: fractional/out-of-range index, masks outside 0/1, malformed/incomplete constants, invalid keys, explicit null, no patch fields. Assert native state, package dirty state and transaction count are unchanged; do not test only errors.
- [ ] Add mismatch/ignored-expression diagnostic assertions. Legacy connected UseConstant=true warns; disabled-inline inputs and selected new translator do not falsely warn. Seed bCanMaskedBeAssumedOpaque=true with authored Masked; effective Opaque and mismatch warning; explicit repair clears mismatch.
- [ ] Full build, then focused InputState filter. Record RED assertions from actual compiled tests; compilation failure alone is not behavioral Red.
- [ ] Implement shared mapping and serialization, fully staged typed patch, native Connect mask copying and root constant policy. Validate source index before side effects. Keep transaction alive across Modify/PreEditChange/mutation/PostEditChange; Modify material/editor data. Preserve existing batch dirty tracking without duplicate save/compile.
- [ ] Declare include_input_state/properties and new command fields as designed. Implement effective blend naming using a complete enum helper without changing established authored names for existing enum values. Compute warnings even when input projection is omitted.
- [ ] Rebuild and rerun focused filter; require all named behavior assertions pass without unexpected diagnostics. Run a disposable public-route smoke of set/read/connect/disconnect and inspect native state. Commit only Task 1 paths with message `fix(material): preserve root input semantics and expose input state`.

### Task 2: Nested property paths with atomic mutation and lifecycle proof

**Files:** AssetOps.cpp; InputStateTest.cpp; InputUtils only where shared policy requires it.
**Consumes:** Task 1 mapping/typed patch validation and existing ResolvePropertyPath/PropertyToJson/JsonToProperty.
**Produces:** get_material(properties=[...]) and extended set_material_property, including owned-object transaction handling.

- [ ] Add failing tests: selected EditorOnlyData.OpacityMask.UseConstant and nested constant members; top-level bCanMaskedBeAssumedOpaque; scalar false/zero readback; ordinary nested structs; unknown/null/external-owner hop refusal; root Expression/whole-input replacement refusal. Verify selected reads do not return partial properties on an invalid selector.
- [ ] Add invalid conversion and root-leaf bypass tests. Invalid request preserves material/editor data, other inputs, dirty state and transaction count. Nested constant members reuse the same finite/range/policy checks as typed patching.
- [ ] Add undo/redo tests for root connect, typed state patch and EditorOnlyData leaf write. Save fixture, release/reload via supported lifecycle and assert exact constants/masks/connection/effective mode. Cleanup only fixture objects/packages and persisted files, with guarded loading.
- [ ] Build and run focused filter; record behavioral RED.
- [ ] Resolve explicit EditorOnlyData prefix through the native getter; validate every object hop stays owned by the material before calling shared resolver. Track reached owner for Modify/edit notification. Convert to initialized temporary property storage; commit only after successful full validation. Preserve existing top-level authoring and route root-state leaf fields through Task 1 policy.
- [ ] Rebuild and run focused filter and public routed nested-read/write/undo/save-reload smoke. Commit Task 2 paths with message `feat(material): support owned nested material property paths`.

### Task 3: MCP completeness and synchronized discovery

**Files:** response.py; test_response.py; capabilities fixture; generated fallback. Change router/profile/cache only if source-confirmed existing routing misclassifies the new set command or ignores selectors.
**Consumes:** approved exact public contract; Task 1 native declarations for fixture generation.
**Produces:** complete-or-refuse formatting for material.get_material and material.list_connections; discoverable new command/parameters with no new tools.

- [ ] Add response tests using exact small root-state/property/connection records and payloads exceeding MAX_RESPONSE_CHARS. Small payload retains all fields; oversized payload returns RESPONSE_TOO_LARGE and reader_complete=false, not a shortened root_inputs/connections array. Guidance names supported narrowing and does not advertise limit/offset.
- [ ] Controller runs `rtk proxy uv run pytest tests/test_response.py -k material -v` with cwd Plugins/UnrealCortex/MCP and records RED.
- [ ] Add both qualified commands to existing complete-or-refuse map. Avoid bespoke routing/pagination infrastructure. Controller reruns focused tests for GREEN.
- [ ] After native declarations stabilize, capture the complete live capabilities through existing local tooling, preserve unrelated fixture fields, and run `rtk proxy uv run python scripts/sync_fallback.py --from-fixture` from MCP. Verify real live operation schemas for new fields, not fixture-only snapshots. Do not add wiring-only permanent tests.
- [ ] Run affected existing response/router/capabilities/schema/composite suites in one controlled invocation: `rtk proxy uv run pytest tests/test_response.py tests/test_routers.py tests/test_capabilities.py tests/test_capabilities_contract.py tests/test_operation_schema.py tests/test_material_composites.py -v` from MCP. Observe actual public material_cmd behavior through a current server process; stale loaded Python is not candidate evidence.
- [ ] Commit Task 3 paths with message `fix(mcp): preserve complete material state reads and discovery`.

### Task 4: Toolkit guidance and required documentation

**Files:** cortex-toolkit/resources/mcp-tool-reference.md; cortex-toolkit/skills/cortex-material/SKILL.md; toolkit docs/plans design/impl pair if required by its existing conventions; plugin design/impl and verification report; relevant parent system reference only if architecture description changes.
**Consumes:** approved contracts; final native/MCP behavior verified before documentation is called shipped.
**Produces:** examples and warnings matching real commands; preserved toolkit ahead commit; scoped toolkit PR candidate.

- [ ] Switch clean toolkit to a new normal fix/material-input-state-184 branch based on origin/main, leaving the pre-existing docs branch/ref untouched.
- [ ] Update existing material guidance/reference: root-state opt-in read, typed patch example, nested EditorOnlyData property example, explicit save, legacy override diagnosis/repair, compiler-aware warning, complete-or-refuse limits, no array-path grammar. Do not add skills, duplicated registries, or claim optional QA support.
- [ ] Use cortex-sync-toolkit and cortex-sync-docs workflow as applicable. Audit source/doc correspondence and obtain separately mandated artifact approval without re-asking publication authority. Run existing relevant toolkit contract checks once after edits; do not invent a new source-text suite.
- [ ] Bind final design/implementation state and verification report to actual results; update parent architecture references only when required and never stage parent gitlinks/user edits. Commit toolkit-only changes separately with message `docs(material): document root input state and effective diagnostics`.

### Task 5: Integrated acceptance, review and merged delivery

**Consumes:** Tasks 1–4 candidates and named acceptance rows; exact base/candidate/dependency identities.
**Produces:** independently exercised acceptance evidence, reviewed and remotely merged implementation/toolkit PRs, accurately completed issue.

- [ ] Full supported target build; run Cortex.Material+ serially. Run affected Python suite from Task 3; rerun only affected checks after corrections. Record expected vs unexpected diagnostics and zero-warning result.
- [ ] Start only a task-owned rendering editor using supported project launcher; confirm project/editor/build identity and fresh MCP server source. Use disposable generic material/black texture/mesh fixture, legacy translator selected on fixture only. Observe visible baseline with constant=1, then actual masked rendering after public connect; inspect all state. Seed legacy opaque flag, observe mismatch, explicitly clear flag and observe rendering/effective-mode repair. Record screenshot/evidence paths.
- [ ] Exercise ordinary/batch mutation, refusals without state changes, undo/redo and save/cold reload. New translator selection must not cause a false ignored-expression warning. Restore no global engine settings; remove only disposable fixtures after proof.
- [ ] Run mandatory applicable live Cortex benchmark after MCP/schema/native changes; retain unrelated findings without expanding the accepted contract unless a required gate is affected.
- [ ] Use requesting-code-review and verification-before-completion. Review candidate against acceptance, project rules and simplicity; confirm consequential findings in source and fix using affected Red/Green proof. No publication by reviewers.
- [ ] Bind documentation-only final head to tested source-tree identity/ancestry instead of rerunning unchanged suites. Use finishing-a-development-branch while retaining authorized merged-delivery route.
- [ ] Stage scoped paths; verify no pre-existing ahead commits leak into publication; normal push and PR creation in each owning repo. Link issue by full URL; do not claim completion until dependencies and implementation have merged. Recheck remote head/base and required checks, merge with expected-head protection; confirm merge SHA and issue state explicitly.
- [ ] Safely fast-forward affected defaults only when possible. Retain task branches if normal deletion refuses; no force authority inferred. Preserve parent dirt/gitlinks and pre-existing toolkit branch. Report actual merged/reviewed/tested revisions, evidence, attribution and remaining limits.

## Self-review and bounded exit

All approved design criteria map to Tasks 1–5: root state/diagnostics/rewiring (1), nested access/undo/persistence (2), MCP completeness/discovery (3), toolkit/docs (4), actual rendering/lifecycle/integration (5). Five Review Focus inputs each have an owning behavioral test/proof. Helper names/types above are shared; no later task invents alternate names. Optional standalone QA, expression disconnect expansion, generic core API and parent gitlink publication remain excluded as approved.

No runtime evidence is claimed by this plan. Exit is all named acceptance/project gates satisfied for the exact candidate, then publish/merge/reconcile—not another unrestricted audit. A genuine prerequisite or verification barrier blocks integration and is reported precisely after completing reachable work.
