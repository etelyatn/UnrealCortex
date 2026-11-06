# UMG Property Binding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development or executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Deliver issue167 plus user-approved creation/replacement through existing UMG inspection and a fully implemented nullable setter.

**Architecture:** CortexUMG owns one private serialized binding reader, identity/fingerprint computation, source construction, and setter. Existing get_widget/get_tree add opt-in metadata; existing umg_cmd routes the setter. Native mutation verifies readback before Blueprint notification; compile/save remain separate.

**Tech Stack:** UE5.8 installed MSVC editor, Unreal native reflection/transactions, existing TCP command routing, Python FastMCP router/response formatter.

**Spec:** docs/plans/2026-10-06-umg-property-binding-design.md (including accepted Failure Mode Analysis).

## Global Constraints

- No new standalone MCP tool, composite, list command, or separate removal command.
- All UObject reads/writes occur on Game Thread through existing command routing.
- Mandatory expected_fingerprint; exact target and duplicate refusal.
- Explicit null clears; object creates/replaces; omitted/empty/malformed binding refuses.
- No implicit compile, save, reload, dependent writes, orphan cleanup, widget/default mutation, graph creation, event delegate authoring, or MVVM extension authoring.
- Preserve complete raw serialized identities, order of retained records, widget objects, hierarchy, style/defaults, and animation state.
- Readback before MarkBlueprintAsModified; never MarkBlueprintAsStructurallyModified.
- Normal branches in Plugins/UnrealCortex; no worktrees/reset/clean/stash/force-push. Preserve parent changes.
- All shell commands through rtk; serialize UE tests and task-owned Editor usage.
- TDD red then green, zero raw warnings. Tests must exercise consumer-visible behavior, not source-text/wiring copies.

## Review Focus

- Broken source GUIDs must retain raw saved names even when GetMemberName resolves to None.
- Legacy property-to-function replacement must clear SourceProperty before compilation.
- Disconnected nested owners, impure function attributes, and disabled Designer bindings must refuse before side effects; null repair remains permitted.
- Cursor-bearing setter requests must refuse rather than return an unrelated cached page.
- Oversized orphan inspection must preserve fingerprint/counts and false completeness without suggesting impossible per-widget recovery.

## Execution and evidence commands

Engine path confirmed D:/UnrealEngine/UE_5.8. Run build using a native PowerShell command through rtk proxy; set DOTNET_ROOT to D:/UnrealEngine/UE_5.8/Engine/Binaries/ThirdParty/DotNet/10.0/win-x64, DOTNET_MULTILEVEL_LOOKUP=0, DOTNET_ROLL_FORWARD=LatestMajor. Invoke UnrealBuildTool.exe CortexSandboxEditor Win64 Development -Project=D:/UnrealProjects/CortexSandbox/CortexSandbox.uproject -WaitMutex -FromMsBuild -NoLiveCoding -NoHotReloadFromIDE. Do not terminate unrelated Editors.

Native focused RED/GREEN: rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.UMG.PropertyBinding+' -Timeout 600 }". Full domain: same command with Cortex.UMG+. Use + not *. Capture raw logs and inspect warnings/errors, not only aggregate status.

Python focused: rtk proxy powershell.exe -NoProfile -Command "& { Set-Location 'Plugins/UnrealCortex/MCP'; uv run pytest tests/test_umg_property_bindings.py tests/test_response.py tests/test_routers.py -v }". Full staged Python runs use existing run-py-tests skill and repository marker stages; report every failure/skip honestly.

Compilation database currently verified: 538 local UnrealCortex entries, zero Mirror entries. LSP references for GetTree/GetWidget identify implementation, private header, and command-handler caller. Regenerate project-root compilation database after new source files and reload clangd before relying on new symbol navigation.

### Task 1: Native serialized inspection and safe nullable authoring

**Files:**
- Create Source/CortexUMG/Private/Operations/CortexUMGPropertyBindingOps.h/.cpp (one private owner; no new exported public class).
- Modify Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp and Source/CortexUMG/Private/CortexUMGCommandHandler.cpp.
- Modify Source/CortexCore/Public/CortexTypes.h only for distinct property binding error constants if existing semantic codes cannot express ambiguity/read failure.
- Create Source/CortexUMG/Private/Tests/CortexUMGPropertyBindingTest.cpp and CortexUMGPropertyBindingTestUtils.h.

**Interfaces:** private static operations AppendInspection(UWidgetBlueprint*, const TSharedPtr<FJsonObject>& Params, const FString* WidgetName, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult& OutError) -> bool; SetPropertyBinding(const TSharedPtr<FJsonObject>& Params) -> FCortexCommandResult. Params carry include_property_bindings; null WidgetName means asset scope. Reader returns ordered canonical JSON, raw identity equality, and whole-array fingerprint. No dependency on animation utilities' unrelated internals.

- [x] Create a generic transient Widget Blueprint fixture with Canvas root, ProgressDisplay ProgressBar, RetainedDisplay ProgressBar, ElapsedValue float variable, and animation metadata. Follow existing WidgetVariable/AnimationBinding fixture cleanup and package lifetime. Use float (PC_Real/PC_Float), not double, for Percent compatibility. For persistent fixtures use KismetEditorUtilities::CreateBlueprint with UWidgetBlueprint/UWidgetBlueprintGeneratedClass, not partially initialized manual generated classes.
- [x] Write RED tests routed through FCortexCommandRouter for opt-in get_tree/get_widget and null-clear preservation. Example consumer assertions:

```cpp
ReadParams->SetBoolField(TEXT("include_property_bindings"), true);
const FCortexCommandResult Read = Router.Execute(TEXT("umg.get_tree"), ReadParams);
TestTrue(TEXT("Serialized binding inspection succeeds"), Read.bSuccess);
// Guard after success before dereferencing result.
SetParams->SetField(TEXT("binding"), MakeShared<FJsonValueNull>());
SetParams->SetObjectField(TEXT("expected_fingerprint"),
    Read.Data->GetObjectField(TEXT("property_binding_state"))->GetObjectField(TEXT("fingerprint")));
const FCortexCommandResult Cleared = Router.Execute(TEXT("umg.set_property_binding"), SetParams);
TestTrue(TEXT("Exact clear succeeds"), Cleared.bSuccess);
TestEqual(TEXT("Only retained record remains"), Blueprint->Bindings.Num(), 1);
TestTrue(TEXT("Designer object identity preserved"), OriginalProgress == Blueprint->WidgetTree->FindWidget(TEXT("ProgressDisplay")));
```

Assert every retained serialized field, target record absence, widget tree/defaults/style and animation snapshot equality; count alone is insufficient. Add broken-source and missing-widget serialized records; get_tree includes both; get_widget filters only records for its exact widget.
- [x] Build and run focused tests; confirm feature-missing failures, not fixture/compiler errors. Record RED log paths.
- [x] Implement typed native reader: ObjectName/PropertyName/FunctionName/SourceProperty/MemberGuid/Kind direct reads; FEditorPropertyPathSegment::StaticStruct exact typed reflected reads of Struct, MemberName, MemberGuid, IsProperty. Validate schema before reading. Serialize raw owner path/name/GUID/discriminator even for unresolved members; resolution status diagnostic only. Raw fields, not GetMemberName, define identity.
- [x] Compute deterministic whole-array signature using normalized length-delimited strings and all raw fields in original order. Reuse native engine SHA256 facilities already used in CortexSafeFileContract rather than copying animation utility custom SHA implementation. Attach MakeObjectAssetFingerprint base plus domain_signature {version:1, scope:umg.property_binding, asset_path, digest}; validate all required guard fields/types and canonical asset path. No optional-field-only guard bypass.
- [x] Extend existing reads only when include_property_bindings is true; validate boolean type before work. Add setter dispatch and live capability metadata. Declare binding as required with supported metadata type permitting null (use existing unconstrained value representation if schema builder has no union; describe object|null explicitly), then enforce exact types in handler. Do not add generic schema infrastructure for one parameter. Existing router preserves nested null.
- [x] Add RED refusal tests: absent/empty/string binding, missing/partial/stale guard, dirty-to-dirty retained-record change, duplicate exact targets, missing widget, invalid target, cursor/limit/offset field presence including null; assert array, status, dirty flag, and transaction state unchanged. Null on valid unbound widget is no-op; no transaction/dirtying. Implement validation before mutation.
- [x] Add RED non-null behavior tests with payloads:

```json
{"binding":{"kind":"property","source_path":["ElapsedValue"]}}
{"binding":{"kind":"function","function_name":"GetElapsedPercent"}}
```

Use existing fixture-generated compatible pure float-return function. Test creation, replacement, identical no-op, nested struct/object property traversal, missing member, unsupported intermediate container, incompatible return type, impure function, disabled binding policy, and legacy SourceProperty-to-function replacement. Each invalid request must preserve state. Non-null missing skeleton/source metadata refuses without implicit compile; caller compiles explicitly when adding source members first.
- [x] Build canonical record from scratch. Resolve property target plus PropertyNameDelegate only. Walk source members starting from self skeleton/generated class through FStructProperty/FObjectPropertyBase types; validate terminal binder compatibility with native FEditorPropertyPath::Validate. Functions require matching delegate signature and FUNC_Const|FUNC_BlueprintPure, empty legacy SourceProperty, and existing graph GUID when available. Honor ArePropertyBindingsAllowed for non-null writes. Unknown binding object fields refuse rather than silently ignore.
- [x] Mutation: snapshot original array and dirty state; open one transaction and Modify; remove at exact index, replace in place, or append; verify actual full serialized array against intended record and unchanged retained records. On failure restore before notification, cancel, verify restoration, and block asset via existing guard on failed restoration. On success notify MarkBlueprintAsModified; use verified post-state for response and refreshed package fingerprint. Null requires absence; equality includes every raw field. No allocations/copies beyond necessary snapshot, canonical serialization, and response construction.
- [x] Add RED/GREEN undo/redo test that restores the exact array/identities; reader schema fail-closed test uses a test helper accepting validated metadata rather than production failure injection solely for tests. If readback failure cannot naturally be induced, exercise restore helper directly with consumer-state assertions and review the success-boundary ordering; do not invent a public fault-injection API.
- [x] Run focused suite then Cortex.UMG+; no warning suppression. Update design implementation notes with observed API decisions; commit scoped native deliverable after independently exercised smoke.

### Task 2: MCP nullable routing, completeness and contract publication

**Files:** modify MCP/src/cortex_mcp/tools/routers.py, MCP/src/cortex_mcp/response.py and capability fallback/generated surfaces located by symbol references/search; create MCP/tests/test_umg_property_bindings.py. Use existing response/router test fixtures. No new tool registration.

**Interfaces:** Task1 command params and response schema exactly as spec. Getter data has nested property_binding_state; setter data has binding, before_binding, reader_complete, fingerprint, changed, compiled, saved. Response formatter must not turn mutation success into a false refusal after side effects.

- [x] Write RED tests for router preventing cursor interception on setter and opted-in reads, including explicit null pagination fields; nested binding null remains null through actual request parsing. Existing connection test fixture can assert commands sent, but contract proof comes from real live routing in Task3; avoid permanent forwarding-only tests.
- [x] Write RED response tests with oversized nested orphan bindings: reader_complete cannot remain true, fingerprints/counts survive, _error=RESPONSE_TOO_LARGE, no impossible pagination suggestion, original data remains unchanged. Also test a large widget tree with small binding-state metadata cannot lose binding completeness metadata silently. Test oversized setter response retains actual mutation outcome/fingerprint and marks any omitted binding identity incomplete rather than claiming no mutation.
- [x] Implement an early UMG branch before generic cursor handling:

```python
is_binding_write = domain == "umg" and command == "set_property_binding"
is_binding_read = domain == "umg" and command in {"get_tree", "get_widget"} and route_params.get("include_property_bindings") is True
if is_binding_write or is_binding_read:
    if any(key in route_params for key in ("limit", "offset", "cursor")):
        return json.dumps({"_error": "INVALID_FIELD", "_message": "Pagination fields are unsupported for property binding operations."})
    # Normal native dispatch, no generic cached cursor interpretation.
```

Reject invalid include flag natively; do not let transport coercion reinterpret truthy strings. Use existing response error handling rather than assuming errors are always in response.data. Inspect conventions first.
- [x] Extend response formatter with a narrow property-binding envelope handler before generic top-level array truncation. Within size limit return untouched payload. Oversized inspection returns honest bounded failure with asset/scope/counts/fingerprint and reader_complete=false. For setter preserve changed/before/post summary/compiled/saved, full guard and clear omission flags; never discard actual outcome. No generic recursive truncation framework.
- [x] Synchronize capability caches/fallbacks, router docstrings, and get_widget/get_tree optional param descriptions using existing generator workflow. Do not update unrelated module declarations or re-pin wording tests. Use cortex-sync-toolkit to identify required downstream docs changes and publication scope.
- [x] Run focused Python tests then staged full Python suite; apply py-test-status. Commit only task-owned paths; no source-copy or mock-echo permanent tests.

### Task 3: Explicit compile/save/reload, live authoring and delivery

**Files:** create Source/CortexUMG/Private/Tests/CortexUMGPropertyBindingPersistenceTest.cpp; update docs/systems/cortex-umg.md and docs/systems/INDEX.md in owning documented repositories as required; add docs/verification/2026-10-06-issue167-property-bindings.md. Temporary runtime scripts/evidence stay Saved and are removed when obsolete; retain verification reports.

- [x] Write RED lifecycle regression using a valid disposable Widget Blueprint. Establish property binding with source GUID, explicitly compile and save initial state. Clear binding through router, assert removed serialized source before deleting any variable, explicitly compile/save, release fixture references, reload fresh, inspect exact post-state and all retained binding/widget/animation invariants. Use package save/unload conventions already proven by UMG persistence fixture; no global CollectGarbage contamination or project map locks.
- [x] Add creation/replacement lifecycle proof: property source, compatible function, and legacy property-to-function transition survive explicit compile/save/reload. Record no warning/error diagnostics and no generated getter override. Test source-variable removal after clear leaves no serialized source reference; do not rely on compiler sanitization. Run focused lifecycle suite after rebuild and full Cortex.UMG+.
- [x] Run actual task-owned Editor with supported build, discover live capability schema and connect actual MCP stdio/router. Create one generic disposable ProgressDisplay fixture with structured Blueprint/UMG commands where supported; use trusted editor Python only for fixture setup not protected serialized Bindings mutation. Exercise get_tree/get_widget opt-in reads, create/replace/clear, stale guard, duplicate refusal where safely fixture-created, invalid null/empty distinction, separate compile/save, fresh reload. Capture actual JSON plus persisted readback. No project-specific migration logic.
- [x] Apply applicable live MCP benchmark skill for routed/schema changes, covering existing tool discovery and required domains with candidate-bound timings; distinguish native test evidence from live positive behavior. Close only task-owned Editor through its API before native runner starts.
- [x] Complete acceptance ledger with exact candidate/base SHAs, raw log paths, test totals, warning/error counts, exercised live cases, limitations (UE5.6–5.7 not run unless available), and reporter attribution. Refresh evidence after consequential edits. Update implementation checklist/results, module docs and toolkit sync as required; no issue closure before full scope proved.
- [x] Request one independent whole-candidate review with local review package, check findings against source, fix supported blockers and rerun affected evidence. Use verification-before-completion and finishing-a-development-branch; preserve user-selected end-to-end route.
- [x] Confirm remote base/head, push normally, open PR linking #167, resolve required checks, merge exact verified revision using full expected-head protection. Confirm remote merged SHA and issue completed state; cross-repository reconciliation explicit.

Post-publication workspace cleanup policy: fast-forward clean task-owned default branches safely. Normal deletion only for preserved task branches; retain if squash ancestry refuses, no force deletion without authority. Preserve parent dirty gitlinks/schema/cdb. Record actual cleanup outcomes in the completion report rather than claiming them before execution.

## Plan self-review

Every original issue criterion is mapped in Task1/Task3; expanded authoring and nullable semantics are Task1/Task3. All six accepted elicitation findings map to source validation, pre-notification readback, policy/legacy tests, and Task2 routing/response behavior. No placeholder implementations or public fault injection. Runtime and integration remain release gates, not promises of current completion.

## Simplicity check

- [x] Think before acting — approved spec, inspected source, verified elicitation findings, and current base drive this plan.
- [x] Simplicity first — existing read surfaces/router, one private native owner, narrow response handling, no generic CRUD/schema framework.
- [x] Surgical changes — exact binding behavior, required integration/docs/tests; unrelated user work preserved.
- [x] Goal-driven execution — native RED/GREEN, domain acceptance and actual MCP lifecycle exercised; final review/publication remain separate gates. See docs/verification/2026-10-06-issue167-property-bindings.md for the optional broad-run failure and explicit visual waiver.

## Implementation observations

- Engine UWidgetBlueprint::GetRelevantSettings selects UUMGEditorProjectSettings; policy tests configure the effective settings, not the base settings CDO.
- Whole-array identity uses engine OpenSSL SHA256 through a private UMG dependency. Generic FPlatformMisc SHA256 is not implemented on this installed Windows engine; the existing Core safe-file helper hashes files only.
- Blueprint fingerprints include compiled_signature_crc when available. Required native guard fields use exact JSON types before comparison; Unreal JSON convenience getters otherwise coerce numbers/booleans and can admit a destructive clear.
- Validate native NAME_SIZE bounds before any name conversion or mutation-guard path lookup. The setter owns this ordering; the domain dispatcher enters it before the generic guard. Raw orphan ObjectName strings remain fully inspectable through the existing exact string widget lookup.
- Native readback compares full serialized records before notification. Explicit compile/reload can normalize source-path owner classes; lifecycle assertions compare retained records to the current compiled/saved baseline, not an earlier pre-compile representation.
- No-op/refused requests must not consume an undo step. The undo regression places both after a real clear and verifies one undo restores the complete original array.

## Execution results and release gates

Tasks 1 and 2 are committed and complete. Task 3's native persistence fixture shipped with Task 1 under the recorded ruling; live fresh-process persistence, the manual all-domain MCP benchmark, toolkit/project documentation and exact user-approved fixture cleanup are exercised.

After one independent final review and one supported correction pass, the complete native UMG gate passed 81/81 with zero raw diagnostics; non-live Python passed 970/970, actual live E2E/scenario/stress selection 263 passed/2 existing skips. Stage markers overlap and must not be added as unique totals.

An optional rendering Cortex+ run crashed in engine GC during Blueprint RecoveryFaults before the queue reached UMG; it is not a passing release gate. Source-backed structural independence and the independent reviewer support separate unresolved-cause issue #168 under the user's conditional instruction. Verified feature merged in PR169 and issue167 is closed COMPLETED. User explicitly accepted API-only graph evidence after visual activation was refused, and selected exact task fixture deletion. No unrelated benchmark mismatch was silently fixed.

Final review found two Important/P2 defects, no Critical/Minor: case-mismatched widget selectors could mutate, and malformed inspection flags could return cached pages. Failing-before regressions and minimal corrections committed in `3801aaa80d23419757e7999aca3923959c1d1b81`. Actual stdio MCP exercised both corrections and the full author/replace/clear/source-retirement lifecycle; fresh Editor73256 verified persisted absence plus retained raw bindings, hierarchy, defaults, styles and animation. Saved file30682bytes SHA256 `5cc484dd107eaee2ea0f36032e00fb03b6cc914983eea057cf5569f24edb49ab` matched after owned Editor37208 exited and complete live acceptance. Durable evidence: `docs/verification/assets/issue167/review-fix-evidence.json`.

Publication: UnrealCortex PR169 merged `8f6012a5536b880912f59eadd4e06ad8be7549d0` from exact expected head `562373b4fcb4a234655dc4f0ab104dc3199c6a1f` against unchanged base `c05e89bd8aaae5b21af7078ab1a4dfbbeeb8490e`. Remote MERGED and issue167 CLOSED/COMPLETED confirmed. Exit0 Source/MCP/uplugin diff against tested `3801aaa80d23419757e7999aca3923959c1d1b81` plus ancestry binds the shipped result to exercised evidence. Toolkit PR66 and CortexSandbox PR116 merged their exact reviewed documentation heads; no parent gitlink/schema/cdb changes included.


## PR171 compatibility correction

Retain Boshchuk's `const auto&` request-field iteration and existing `JsonKeyToString` conversion without changing binding validation. Current-main integration `cafdecd7912ebb3890264d23cf185060f1e00657` passed the supported editor build, focused real-header Clang RED/GREEN, actual changed translation-unit syntax compilation, 141/141 UMG tests and native TCP positive/refusal smoke. Native tests and smoke had no raw warning/error/fatal diagnostics. Windows Clang checks retain engine-header warnings and are not Linux linking proof; contributor Linux results remain attributed.

Detailed identity, commands, limitations and review: [PR171 verification](../verification/2026-10-08-pr171.md). No MCP/toolkit contract change or new permanent test was required.
