# UMG Widget Animation Authoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development or executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. CortexSandbox forbids worktrees; use the existing normal task branch.

**Goal:** Deliver issue176 through guarded native float/color animation authoring, exact inspection, cold persistence and observable saved-widget playback, followed by reviewed merge and issue reconciliation.

**Architecture:** Two new routed umg commands extend the existing serialized UMG/MovieScene owner. A private track-content helper serves native validation/readback and existing inspection; the existing fingerprint owner cuts over to version 2. MCP handling stays in the existing router/response modules and preserves write outcomes independently of detailed inspection.

**Tech Stack:** Unreal Engine 5.8, C++/UObject/MovieScene/UMG, Python/FastMCP, pytest, Windows rendering automation.

**Spec:** [2026-10-08-umg-animation-authoring-design.md](2026-10-08-umg-animation-authoring-design.md), explicitly approved by the user after both independent review lenses confirmed corrections. Source baseline: 6506e03a21a8a2c6af1570b3d01e736947cd3040. Design commit: b6f32c2.

**Status:** Implementation and mapped independent source review complete. Controller verified rendering Cortex+1802/1802, nonlivePython993/993, actualSDKauthor31/31 and fresh-processcold19/19 including real widget playback. Prescribed cross-domain benchmark complete:249actualSDKcalls and viewed graph/Designer/material/scene surfaces; task-owned fixtures/editor cleaned up. Evidence: [verification report](../verification/2026-10-08-issue176-umg-animation-authoring.md). Publication and expected-head merged delivery are in progress. Historical step checkboxes below are the approved execution brief; the ledger and verification report carry actual checkpoint results.

## Global Constraints

- All shell commands through rtk. No worktrees, reset, clean, force push, force deletion, or termination of unrelated editors/services.
- Preserve parent plugin/toolkit gitlink modifications. Commit implementation only in the owning plugin repository; toolkit changes belong to its own normal branch and scoped PR.
- All UObject access and mutation on the Game Thread; CortexUMG remains the only domain owner. Existing MovieScene/UMGEditor dependencies suffice.
- No implicit compile, save, reload, retry or target repair.
- Ordinary named Designer widgets only. Designer root is an ordinary widget, not bIsRootWidget=true.
- Scalar float and FLinearColor tracks only; linear or constant keys. No raw property writes, event/custom/slot/dynamic authoring or composite-schema expansion.
- dry_run defaults true; explicit track:null clears exactly one property track and retains the binding/possessable.
- Quantize with TickResolution.AsFrameTime(seconds).RoundToFrame(); check representability before conversion. Half-open [S,E) evaluation ranges; keys at E are stored control points, not guaranteed samples.
- Canonical Absolute/RestoreState/active sections with zero easing/preroll/postroll; unset channel defaults, constant infinity extrapolation, native tick rate, normalized float precision and tangent state.
- At most 8 sections and 64 logical keys per track; prospective normalized mutation response conservatively fits 32,000 Python pretty/ASCII characters. Detailed MCP response ceiling is 40,000 characters.
- umg.animation_binding signature version 2; version-1 guards refuse, with no alias or shim.
- Failing-first behavior coverage before production implementation. Controller runs RED/GREEN and integrated acceptance; editing agents skip builds, tests, lint and formatters mid-flight.
- Serialize native runners and task-owned manual editor processes. Use Cortex.UMG+ (plus wildcard), rendering enabled, Timeout600.
- UE5.8 is locally available; UE5.6/5.7 runtime compatibility is unverified and must be stated.

## Review Focus

Each row is a consumer-visible risk and has explicit coverage below, not an extra feature.

1. Unbound target changes between read and ensure must stale-refuse even on dirty-to-dirty edits — Task 1 tests the existing whole-WidgetTree digest coverage.
2. Existing matching keys with additive/eased/defaulted evaluation must not yield changed=false — Task 1 normalizes the entire evaluation state and tests actual midpoint effects.
3. Adjacent sections and exclusive playback endpoint must not extend or overlap ranges — Tasks 1 and 3 test native frame bounds and last-valid-tick playback separately.
4. Cached read cursor or null pagination supplied to a write must not produce a cached success — Task 2 verifies explicit INVALID_FIELD before any mutation.
5. Oversized detailed read or applied mutation must not erase completeness/outcome information — Task 2 covers both many-track and single-binding overflow and preserves mutation/recovery envelopes.

## File and edit ownership map

All paths below are plugin-relative unless prefixed with workspace:.

| Owner | Files | Responsibility |
| --- | --- | --- |
| Native implementer | Source/CortexUMG/Private/Operations/CortexUMGAnimationAuthoringOps.h/.cpp (new) | Strict command preflight, native ensure/set/clear, transaction journal and verified recovery |
| Native implementer | Source/CortexUMG/Private/Operations/CortexUMGAnimationTrackUtils.h/.cpp (new) | Focused native float/color content extraction, equality and conservative pretty-response budget accounting shared by inspection/authoring |
| Native implementer | Source/CortexUMG/Private/Operations/CortexUMGAnimationBindingUtils.h/.cpp | Version-2 canonical property/evaluation coverage, stale checks and readable-state diagnostics |
| Native implementer | Source/CortexUMG/Private/Operations/CortexUMGWidgetAnimationOps.cpp | Opt-in detailed existing binding inspection, complete/incomplete envelopes |
| Native implementer | Source/CortexUMG/Private/CortexUMGCommandHandler.cpp | Dispatch, mutation guard admission and live schema publication |
| Native implementer | Source/CortexUMG/Private/Tests/CortexUMGAnimationAuthoringTestUtils.h; CortexUMGAnimationAuthoringTest.cpp; CortexUMGAnimationAuthoringLifecycleTest.cpp (new) | Generic fixture and meaningful command/frame/preservation/undo/recovery/playback regressions |
| Native implementer | Existing CortexUMG animation-binding test files | Semantic guard cutover; remove incidental metadata-only assertions instead of re-pinning them |
| MCP implementer | MCP/src/cortex_mcp/tools/routers.py; response.py | Uncached direct mutation routing, native errors, complete detailed reads and bounded write outcome preservation |
| MCP implementer | MCP/tests/test_umg_animation_bindings.py | Behavioral pagination and response-boundary regressions; delete affected mock-echo/wiring-only tests |
| Controller after live build | MCP/tests/fixtures/capabilities_cache_full.json; MCP/src/cortex_mcp/_fallback_generated.py | Refresh only affected live contracts; generate fallback, never hand-edit generated output |
| Controller | This plan, approved design, plugin docs/verification/2026-10-08-issue176-umg-animation-authoring.md (new after proof) | Execution ledger, decisions and exact candidate evidence |
| Toolkit synchronizer after proof | cortex-toolkit/resources/umg-patterns.md and affected existing command reference/discovery files | Implemented command examples and contract synchronization; no unrelated skill changes |
| Controller after proof | workspace:docs/systems/cortex-umg.md; workspace:docs/systems/INDEX.md | Targeted architecture/command documentation, including correcting binding_guid selector spelling |

Do not split the native journal/fingerprint/content helper across concurrent writers. Python router/response edits are independent after the approved contract; discovery fixture regeneration waits for the native live schema. The controller owns integration and verification.

## Shared native interfaces

Keep these private; no exported plugin API or new public module types.

```cpp
class FCortexUMGAnimationAuthoringOps
{
public:
    static FCortexCommandResult EnsureAnimationBinding(const TSharedPtr<FJsonObject>& Params);
    static FCortexCommandResult SetAnimationPropertyTrack(const TSharedPtr<FJsonObject>& Params);
};

namespace CortexUMGAnimationTrackUtils
{
    // False means native content is not completely supported/readable, not empty.
    bool DescribeTrack(UMovieSceneTrack* Track, const FFrameRate& TickResolution,
        TSharedPtr<FJsonObject>& OutContent, FString& OutReason);
    // Conservative upper bound for json.dumps(indent=2, ensure_ascii=True).
    bool FitsResponseBudget(const TSharedPtr<FJsonObject>& Object, int32 MaxChars);
}
```

The authoring implementation uses private typed parsed keys/sections to compare requested frames/native float values and evaluation fields directly against native channels; do not serialize JSON strings merely to test equality. The JSON extraction exists because clients require readback, not as a second authored-data source. Fingerprinting remains in CortexUMGAnimationBindingUtils and must not use editor display labels as property identity.

## Commands for controller-owned checkpoints

Use the configured engine at D:/UnrealEngine/UE_5.8, confirmed by workspace .cortex/config.yaml. Keep build output for each RED/GREEN checkpoint.

Build from workspace root:

```bash
rtk bash -lc 'ENGINE_PATH=$(python cortex-toolkit/lib/cortex_config.py --project-dir . --get engine.path | tr -d "\r"); export DOTNET_ROOT="$ENGINE_PATH/Engine/Binaries/ThirdParty/DotNet/10.0/win-x64" DOTNET_MULTILEVEL_LOOKUP=0 DOTNET_ROLL_FORWARD=LatestMajor; rtk proxy "$ENGINE_PATH/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" CortexSandboxEditor Win64 Development -Project="$(pwd)/CortexSandbox.uproject" -WaitMutex -FromMsBuild -NoLiveCoding -NoHotReloadFromIDE'
```

Native focused/full, one at a time:

```bash
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.UMG.AnimationAuthoring+' -Timeout 600 }"
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.UMG+' -Timeout 600 }"
```

Python from plugin MCP:

```bash
rtk proxy uv run --no-sync pytest tests/test_umg_animation_bindings.py -v
rtk proxy uv run --no-sync pytest tests/ -m "not e2e and not scenario and not stress" -v
rtk proxy uv run --no-sync python scripts/sync_fallback.py --from-fixture
rtk proxy uv run --no-sync python scripts/sync_fallback.py --from-fixture --check
```

The installed cortex-mcp.exe was locked during an ordinary uv run's synchronization attempt. Use --no-sync for the existing environment; diagnose missing dependencies if encountered without terminating that process. Rebuild before trusting automation after source changes. Classify raw severity-tagged diagnostics, not just the runner summary.

---

## Task 1: Native guarded binding/property authoring and exact inspection

**Files:** Native-owned files in the map above; no MCP edits or commits to workspace parent.

**Consumes:** Approved JSON contracts; existing ParseSelector, ComputeFingerprint/VerifyFingerprint, guarded Widget Blueprint loader, asset mutation guard and native MovieScene APIs.

**Produces:** Both commands, version-2 guard, include_track_content readback, and native tests under Cortex.UMG.AnimationAuthoring+.

### Step 1 — Add failing public-command regression coverage

- [ ] Create a task-local fixture using FKismetEditorUtilities::CreateBlueprint with UUserWidget parent, UWidgetBlueprint asset class and UWidgetBlueprintGeneratedClass generated class. Give it a CanvasPanel Designer root, Decoration Image and Unaffected Image; register a real FCortexUMGCommandHandler with FCortexCommandRouter. Create Fade via existing umg.create_animation, length=0.1. Do not manually populate bindings/tracks: all authored contents must go through the new commands.
- [ ] Fixture helpers provide BaseParams(), Inspect(bool bDetailed), Ensure(const FString& WidgetName, bool bDryRun), Set(const TSharedPtr<FJsonObject>& Selector, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Track, bool bDryRun), Animation(), Blueprint(), Router(), and CaptureAuthoredState(). Ensure/Set obtain a fresh read unless a test explicitly supplies a stale guard. Keep unique packages and strong refs; cleanup clears fixture-owned public/standalone flags, retires assets/classes/CDOs safely and lets reflection metadata die through normal GC.
- [ ] Add a JSON helper parsing fixed test payloads with FJsonSerializer/TJsonReaderFactory. Refuse malformed test setup through AddError; do not insert fake fallback values.
- [ ] Add the primary behavioral test body below using those fixture methods. The test file includes the helper and native section/channel headers, never an unimplemented production header.

```cpp
FCortexUMGAnimationAuthoringFixture Fixture(*this);
const FCortexCommandResult Bound = Fixture.Ensure(TEXT("Decoration"), false);
TestTrue(TEXT("Ordinary widget binding created"), Bound.bSuccess);
if (!Bound.bSuccess || !Bound.Data.IsValid()) return false;
const TSharedPtr<FJsonObject> Selector = Bound.Data->GetObjectField(TEXT("matched_selector"));
TestFalse(TEXT("Designer binding is not the UserWidget root"),
    Selector->GetBoolField(TEXT("is_root_widget")));
const TSharedPtr<FJsonValue> Fade = Fixture.JsonValue(TEXT(R"JSON(
{"type":"float","sections":[{"start_seconds":0,"end_seconds":0.1,"keys":[
{"time_seconds":0,"value":0,"interpolation":"linear"},
{"time_seconds":0.1,"value":1,"interpolation":"linear"}]}]}
)JSON"));
const FCortexCommandResult Written = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
TestTrue(TEXT("Opacity track authored"), Written.bSuccess);
if (!Written.bSuccess) return false;
UMovieSceneFloatTrack* Track = Fixture.FloatTrack(TEXT("Decoration"), TEXT("RenderOpacity"));
TestNotNull(TEXT("Native property track exists"), Track);
if (!Track || Track->GetAllSections().Num() != 1) return false;
UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
TestNotNull(TEXT("Native float section exists"), Section);
if (!Section) return false;
float Midpoint = -1.0f;
Section->GetChannel().Evaluate(Fixture.Animation()->MovieScene->GetTickResolution().AsFrameTime(0.05), Midpoint);
TestEqual(TEXT("Fade evaluates to the intended midpoint"), Midpoint, 0.5f);
TestTrue(TEXT("Playback endpoint stays exclusive"), Section->GetRange().GetUpperBound().IsExclusive());
const FCortexCommandResult Repeat = Fixture.Set(Selector, TEXT("RenderOpacity"), Fade, false);
TestTrue(TEXT("Repeat authoring succeeds"), Repeat.bSuccess);
TestFalse(TEXT("Identical canonical content is not rewritten"), Repeat.Data->GetBoolField(TEXT("changed")));
```

Define FloatTrack(widget,path) in the fixture by traversing native binding GUID -> native property tracks and comparing exact property path, not display names. Add fixture method JsonValue(const FString&) using the strict JSON helper above; these helpers are test-local and have no production role.

- [ ] Add color keys with RGBA endpoints {0,0,0,1} -> {1,0.5,0.25,1}; assert four independently inspectable native channels at midpoint {0.5,0.25,0.125,1}. Include an existing color section with unequal channel times in inspection and preserve those independent times.
- [ ] Add wrong-case widget/animation/selector/path refusals, duplicate/shared/dynamic binding refusals, missing/wrong possessable class, null-clear without binding deletion, replacement preserving other tracks and sibling animations, preview with unchanged authored bytes and dirty state, and idempotent ensure.
- [ ] Add time boundaries: adjacent [0,.05)/[.05,.1), duplicate quantized times, repeated times, NaN/infinity/native float overflow, invalid rate, frame overflow, negative/out-of-playback times and sections quantizing to zero duration. Compare native bounds and values, not source text.
- [ ] Add dirty-to-dirty stale cases for target class/name change before ensure, property path-only edits, channel keys and evaluation-default changes. Old version-1 guards must refuse even when their digest text is copied from a current read.
- [ ] Add same-key/noncanonical blend/easing/extrapolation/default replacement, required response budget refusal, undo/redo and injected failed apply/readback recovery. Recovery tests assert exact selected/retained authored state, dirty flag and correct hard-failure admission behavior; use test-only native injection reset by RAII.

### Step 2 — Controller proves RED

- [ ] Build the test-only patch, then run Cortex.UMG.AnimationAuthoring+. Expected failures are missing new command/absent authored behavior, not fixture warnings, compile errors or invalid existing animation setup. Correct fixture-only failures before production edits.
- [ ] Save RED log paths and exact failing assertions in this task's execution record. Test-writing subagent waits at this barrier and does not implement ahead of it.

### Step 3 — Implement validated native commands and reusable inspection

- [ ] Add declarations above, register both command names in Execute and GetSupportedCommands, and route both through the existing failed-recovery mutation guard before side effects. Reject supplied unknown fields and pagination fields, including null, at native validation too.
- [ ] Parse JSON into bounded typed records once; validate every required string/object/bool/number before transaction. Resolve exact animation, exact ordinary widget/selector and compatible typed native property. No CPF_Interp requirement. Read guard once and verify before writes; cache that validated result for preview instead of recomputing the whole animation unnecessarily.
- [ ] Build the ordinary binding using native semantics already confirmed in UE5.8 WidgetAnimation.cpp:233-278:

```cpp
const FGuid Guid = MovieScene->AddPossessable(Widget->GetName(), Widget->GetClass());
FWidgetAnimationBinding Binding;
Binding.WidgetName = Widget->GetFName();
Binding.SlotWidgetName = NAME_None;
Binding.AnimationGuid = Guid;
Binding.bIsRootWidget = false;
Animation->AnimationBindings.Add(Binding);
```

This code executes only inside the guarded transaction after all validation. Existing healthy binding returns its actual selector without new GUID generation. Preview of creation returns matched_selector:null and planned_target, not a fabricated applied GUID.

- [ ] Build native float/color track objects under MovieScene with RF_Transactional, sections under their track, exact property name/path and canonical evaluation fields. Normalize frames and values before allocation. Populate channels in one pass using checked bounded Times/Values arrays rather than repeated key-array copying:

```cpp
FMovieSceneFloatValue Value(NormalizedValue);
Value.InterpMode = bConstant ? RCIM_Constant : RCIM_Linear;
Value.TangentMode = RCTM_User;
Value.Tangent = FMovieSceneTangentData();
Times.Add(Frame);
Values.Add(Value);
// After all validated keys for this channel:
Channel.Set(MoveTemp(Times), MoveTemp(Values));
Channel.SetTickResolution(MovieScene->GetTickResolution());
Channel.PreInfinityExtrap = RCCE_Constant;
Channel.PostInfinityExtrap = RCCE_Constant;
Channel.RemoveDefault();
```

Use four native color channels in their r/g/b/a order. Do not impose equal times when reading preexisting color data. Do not clamp values, auto-expand playback range or synthesize cubic tangents.

- [ ] Implement DescribeTrack by reading exact property identity, native section bounds and evaluation state, float/color channels/defaults/extrapolation/tick rates. Return content_supported=false with reason on unsupported overrides/custom states, not fabricated keys. Existing inspection gets include_track_content strict boolean validation and the complete/incomplete envelopes in the spec.
- [ ] Implement conservative pretty-response accounting over the bounded JSON tree, not a compact-JSON-length guess. Count escaped strings with a maximum six characters per UTF-16 code unit, quoted keys, punctuation, indent/newlines and a conservative 32-character finite-number bound. A short-circuit recursion stops once MaxChars is exceeded; no intermediate pretty string or repeated serialization allocation is needed. Include fixed-length worst-case new GUID/digest fields in the prospective create response. Check MaxChars=32000 before Modify; detail extraction also stops before unbounded payload allocation.
- [ ] Extend canonical serialization with native property name/path, track evaluation flags, section overlap/evaluation state and channel overrides affecting supported authoring. Existing whole-tree target guard is reused (AnimationBindingUtils.cpp:524-550). Producer/verifier both use version2. Reject incomplete mutation guard coverage; keep inspection diagnostics available. Remove incidental version-only assertions and migrate semantic caller fixtures, with old-version refusal coverage.
- [ ] Journal FMovieSceneBinding membership and targeted track/section references with GC-safe strong references, plus prior binding records/dirty state. Native APIs confirmed through clangd and source: AddGivenTrack(Track,Guid), RemoveTrack(*Track), ReplaceBinding(Guid,SavedBinding). Saving the original binding preserves track order and unrelated membership:

```cpp
const FMovieSceneBinding SavedBinding = *MovieScene->FindBinding(Selector.BindingGuid);
const bool bWasDirty = Blueprint->GetPackage()->IsDirty();
// Failed mutation/readback path restores the original binding membership:
MovieScene->ReplaceBinding(Selector.BindingGuid, SavedBinding);
Blueprint->GetPackage()->SetDirtyFlag(bWasDirty);
```

For newly created bindings, restore the saved UMG records and remove only the newly added possessable. These snippets are journal operations, not the complete recovery proof: independently re-read and compare exact restored authored state before cancelling and reporting rolled_back=true. Hold removed objects through recovery/undo; never mark a track garbage while a committed undo/redo transaction still owns it. Unproven recovery blocks subsequent writes through the existing asset mutation guard and returns DIRTY_EDITOR_STATE with rolled_back=false.
- [ ] Modify Blueprint, Animation, MovieScene and existing affected transactional objects before writes. Verify selected native readback and retained authored state before one Blueprint notification. Cancel only after actual restoration on failure. Idempotent paths do not Modify, notify, compile or save. No extra domain dependency or new rollback-safe batch declaration.

### Step 4 — Controller proves GREEN and reviews the native slice

- [ ] Rebuild; run focused authoring then complete rendering Cortex.UMG+. Confirm assertion totals and zero unexpected warnings/errors/ensures in raw logs.
- [ ] Independently call ensure/set/inspect through the actual TCP/MCP route after the MCP slice is integrated; native tests alone do not complete delivery.
- [ ] Source-check an independent reviewer's consequential findings; apply scoped corrections and refresh affected tests before committing the native slice. Update this ledger with actual source SHA/logs, not an agent's passing claim.

## Task 2: MCP guarded dispatch, precise errors and bounded responses

**Files:** MCP-owned files in the ownership map. Discovery fixture/generated fallback changes are deferred until live native registration exists.

**Consumes:** The approved native names/envelopes and include_track_content flag; existing strict router wrapper, pagination cache and UECommandError.

**Produces:** Safe actual umg_cmd routing and consumer-visible boundary regression coverage. This slice can be edited independently of Task 1; tests are controller-owned and no overlapping source files.

### Step 1 — Write failing behavioral boundary tests

- [ ] Parameterize both new writer names with cursor/offset/limit including null, True, 1.5 and a valid cursor originating from another cached read. Existing cache creation and encode_cursor pattern is in test_umg_animation_bindings.py:79-102. Assert INVALID_FIELD, no unrelated cached rows and no forwarded mutation. This is a refusal invariant, not a forwarding/mock-echo test.
- [ ] Add an oversized authoring result with changed=true, actual selector, version2 fingerprint and a >40k authored_track; assert applied outcome and fingerprint survive, reader_complete=false and authored_track_omitted=true. Add a recovery failure with rolled_back=false and verify the native error and blocked-state facts survive formatting.
- [ ] Add detailed pages with six tracks whose total payload exceeds 40k and a single oversized binding; assert explicit RESPONSE_TOO_LARGE/reader_complete=false, retained counts/fingerprint and no complete partial page. Existing summary-only behavior remains unchanged.

```python
@pytest.mark.parametrize("command", ["ensure_animation_binding", "set_animation_property_track"])
@pytest.mark.parametrize("extra", [{"limit": None}, {"offset": None}, {"cursor": None}, {"limit": True}, {"limit": 1.5}])
def test_authoring_rejects_supplied_pagination(command, extra):
    connection = MagicMock()
    router = strict_router_tool(make_router("umg", connection, "test"), "umg")
    result = json.loads(router(command, {"asset_path": "/Game/UI/Scratch", "animation_name": "Fade", **extra}))
    assert result["_error"] == "INVALID_FIELD"
    assert "rows" not in result
    connection.send_command.assert_not_called()
```

Extend with a real stored cursor from another read as a separate case, not repeated equivalent rows. Keep formatter overflow assertions at the actual router output rather than only a private helper.

### Step 2 — Controller proves RED

- [ ] Run the focused Python file with --no-sync before production edits. Missing guarded dispatch and lost outcome/completeness must fail. No source-text, metadata-copy or not-throw tests.

### Step 3 — Implement routing and bounded formatting

- [ ] Handle both writes before cursor/limit dispatch; reject field presence regardless of value. Call uncached native transport and propagate UECommandError and returned native failure envelopes rather than extracting absent data as empty success.

```python
authoring = domain == "umg" and command in {
    "ensure_animation_binding", "set_animation_property_track",
}
if authoring and any(key in route_params for key in ("limit", "cursor", "offset")):
    return json.dumps({"_error": "INVALID_FIELD", "_message": "Pagination fields are unsupported for animation authoring."})
```

Complete the branch with the existing transport and dedicated formatting in response.py; do not create a new public MCP tool or wrap generic pagination.

- [ ] Add optional command/detail context to existing format_response while preserving all existing callers. New write formatting retains all applied fields specified in the design and any error/recovery fields; omit only detailed authored_track if oversized. Never emit a generic size refusal for an already applied mutation.
- [ ] For include_track_content=true reads, check complete serialized response size before generic nested-track truncation. Return complete page or exact incomplete summary envelope. Strict boolean validation of include_track_content happens before dispatch; summary-only reads retain their established behavior.
- [ ] Remove existing tests whose only assertion is forwarding unchanged copied values or incidental wording/metadata, rather than updating their expected copies. Keep genuine refusal/overflow/pagination invariants. No unrelated pagination refactor.

### Step 4 — Controller proves GREEN and reviews the MCP slice

- [ ] Run focused Python tests and non-e2e/scenario/stress suite using required Python test/status skills. Exercise actual registered umg_cmd requests against the built native plugin, including a stale guard, wrong-case target, explicit-null clear and detail content.
- [ ] Independently review native/Python shape agreement; fix supported findings and refresh affected evidence. Commit scoped MCP changes only after controller proof.

## Task 3: Saved-widget lifecycle, independent acceptance and discovery

**Files:** New native lifecycle test file, affected existing native tests, capabilities fixture/generated fallback; verification report created after exercised proof. Throwaway live probes are task-owned and removed after evidence capture.

**Consumes:** Integrated Tasks 1/2, rebuilt editor and actual routed authoring commands.

**Produces:** Proven opacity/color effects, persisted identities/keys and consistent existing rename/removal behavior, plus actual live discovery.

- [ ] Before any lifecycle-only production correction, add/run a failing native regression. Author both tracks via Router, compile explicitly, instantiate the generated UUserWidget and select its compiled animation. Do not play the editor-owned template as a substitute for the compiled saved sequence.
- [ ] Reuse native UMG action/animation tick semantics from existing persistence tests, but avoid their unsafe derived-object downcast. Obtain the inherited protected base member pointer through a test-only access class without constructing that class:

```cpp
struct FCortexWidgetAnimationTickAccess : UUserWidget
{
    static auto Member() { return &FCortexWidgetAnimationTickAccess::TickActionsAndAnimation; }
};
(Widget->*FCortexWidgetAnimationTickAccess::Member())(0.0f);
Widget->FlushAnimations();
(Widget->*FCortexWidgetAnimationTickAccess::Member())(0.05f);
Widget->FlushAnimations();
```

Use the actual returned player/compiled widget's animation, observe Decoration.RenderOpacity=0.5 and expected color midpoint, and compare Unaffected's exact initial values. Include an unplayed instance negative control. This tests real property setter/accessor effects, not just channel evaluation. Inspect raw warnings and lifecycle cleanup.
- [ ] Add last-valid-tick and adjacent-section playback assertions with exact authored/native frames. Completion RestoreState is tested separately from an intermediate sample; do not assert the excluded playback endpoint was evaluated.
- [ ] Rename Decoration through existing reference-aware umg.rename_widget, invoked as umg_cmd(command="rename_widget", params={asset_path, widget_name:"Decoration", new_name:"DecorationRenamed", expected_fingerprint}). Its guard is the whole-tree fingerprint, not the animation or Designer-property-binding fingerprint. On this task-owned fixture, get_widget exposes is_variable; call existing set_widget_variable with that same value, require changed=false and unchanged authored state, and use its returned complete fingerprint including compiled_signature_crc. Then rename, inspect exact new binding name with the same GUID/property tracks and play a freshly compiled instance. Explicit-null clearing one track leaves the other; existing remove_animation_binding removes the last binding's possessable/tracks and preserves unrelated widgets/animations. Undo/redo remains coherent.
- [ ] Controller launches only task-owned editor processes after automation has completed. Use actual stdio MCP client against the explicitly selected CortexSandbox port, inspect live get_operation_schema/profile_operation_schema contracts, create the generic scratch Blueprint via widget_compose, create animation, ensure binding, set tracks, inspect details, explicitly compile and save. Capture exact native-normalized frame/value/interpolation data and package identity.
- [ ] Close that editor through its own API, start a fresh task-owned editor process, reconnect to its discovered port, read the saved asset without reauthoring and compare exact persisted binding GUID/property path/ranges/channel keys. Play the saved compiled animation on a real instantiated widget using trusted local editor automation only for the observation barrier; authoring remains structured MCP. Capture intermediate opacity/color and unaffected sibling values. No polling until a desired value or fake reflected property writes.
- [ ] If mounted MCP remains disconnected, use the plugin's supported stdio server and actual MCP ClientSession/TCP discovery; distinguish harness connection failure from editor transport failure. Never treat one unavailable mounted route as proof that live validation is impossible.
- [ ] Refresh affected fixture contracts from the exact candidate's live capabilities. Run scripts/sync_fallback.py --from-fixture and --check. Do not hand-edit generated fallback or blindly replace unrelated live-cache domains. Existing profile allows umg by domain, so do not invent a new allowlist or profile.
- [ ] Run required live Cortex MCP benchmark for changed schemas/router boundaries in addition to focused acceptance; exercise relevant positive/refusal domains as required by its skill. Record timing/output/actual candidate; do not mark simulated evidence live.
- [ ] Run final build, full rendering Cortex.UMG+ and Python non-e2e/scenario/stress suite serially. Final whole-candidate reviewer receives source/evidence package, spec, this plan and exact head/base. Controller verifies consequential findings and repeats affected acceptance after fixes.

## Task 4: Documentation/toolkit synchronization and merged delivery

**Consumes:** All acceptance rows passing for the exact candidate; explicit merged-delivery authority already established. This task cannot close the issue on a PR merely opened or a dependency landed.

- [ ] After smoke proof, update design/plan status with exercised facts, write the plugin verification report with baseline/tested/reviewed source identities, native/Python counts/raw diagnostics, actual stdio operations, fresh-process persistence and playback evidence. Clearly state UE5.6/5.7 unverified. Remove only task-owned throwaway probes/fixtures after evidence is retained.
- [ ] Apply cortex-sync-toolkit to the implemented contracts and review its edits. Toolkit is initially clean and detached at abb5effa4cc6e8552eda888b3ee1fc0efbd5456c; create a normal task branch before edits. Publish scoped toolkit changes under the selected end-to-end delivery, not the workspace parent gitlinks.
- [ ] Apply cortex-sync-docs' targeted audit/approval gate for project system references. Report proposed CortexUMG/INDEX corrections before applying; unrelated documentation and request statuses are unchanged unless a corresponding request actually exists and is completed.
- [ ] Use requesting-code-review, verification-before-completion and finishing-a-development-branch with the selected merged-delivery endpoint preserved. Inventory task paths and existing ahead commits before staging/publication. Include the reporter @etelyatn and no invented implementation attribution.
- [ ] Push normally; create linked plugin/toolkit PRs describing full accepted scope and real evidence/limits. Confirm checks/review and fresh head/base. If base drifts, refresh affected evidence; documentation-only final commits require source-tree identity/ancestry binding to the tested candidate.
- [ ] Merge only the reviewed/tested remote head with full expected-head protection; confirm actual merge SHA and forge state. Verify issue176 closes as completed only after every accepted criterion is met and required dependencies are integrated. Explicitly reconcile cross-repository links.
- [ ] Fast-forward affected default branches if safe. Delete only task-owned branches with ordinary safe deletion; if squash ancestry prevents normal deletion, retain them without asking for force authority. Leave parent gitlinks/unrelated edits preserved. Report exact reviewed/tested/merged identities, remaining limits and workspace readiness.

## Approval and execution handoff

User approved subagent-driven execution with one native owner and one independent MCP owner, controller-owned RED/GREEN checkpoints and integrated acceptance. The native mutation journal/guard stays with one native writer; Python routing/formatting is a genuinely independent slice with a fixed contract. Architectural artifact gates are complete. Merged-delivery authority remains established.

## Execution record

- Design review: five supported findings patched; both reviewers confirmed no unresolved scope findings. Controller source checks and synthetic existing-formatter smoke are recorded in the design; they are not animation acceptance.
- Written specification: user approved through the explicit written-spec gate.
- Plan self-review: all specification acceptance rows map to Tasks 1–3; ownership, interfaces and five Review Focus cases checked; no unfinished design placeholders. Both existing review lenses stress-tested the plan without executing it.
- Native review correction: MovieSceneFloatChannel.h:197 takes Set arrays by value; the implementation snippet now moves Times/Values instead of copying them. Native reviewer confirmed the reversible track APIs and safe inherited member-pointer tick access.
- Public-contract review correction: native dispatch/schema names rename_widget (CortexUMGCommandHandler.cpp:52-54,190-194), not rename. The controller rejected an unsupported reviewer claim that get_tree exposes its fingerprint: GetTree:354-383 does not. The corrected controlled-fixture workflow gets the unchanged is_variable from get_widget and obtains the proper tree fingerprint from the no-op set_widget_variable response before any transaction (WidgetTreeOps.cpp:1021-1047); the reviewer confirmed that correction. Neither animation nor property-binding fingerprints substitute for the rename guard.
- Both reviewers reported no remaining blockers within their original review scopes after correction. No acceptance criterion was set aside; build/tests/native runtime remain unexecuted.
- Implementation plan: user approved and selected subagent-driven execution. Plan-scoped ledger and exclusive native/MCP edit ownership established; initial task dispatch is tests-only, before controller RED proof. No production authoring/native GREEN/cold-reload/new-animation playback claim.
- Integrated acceptance: full rendering Cortex+1802/1802 on Saved/TestLogs/AutomationTest_2026-10-08_164424.log, no unexpected raw native diagnostics; Python993/993 (265deselected) on Saved/Issue176/mcp_final_nonlive.xml. ActualSDK author31/31 and NEW-process cold19/19 on Saved/Issue176/author_acceptance_final.json and cold_acceptance_final.json, including exact persisted digest/content, native generated-widget opacity0.5/color[0.5,0.25,0.125,1], unaffected/unplayed controls, rename/recompile/replay and surgical clear/removal.
- Harness ordering correction: RunTests.ps1:439-451 deletes Content/Temp/Cortex*Test* before/after native runs; the earlier MCP package was removed by that cleanup, not shown to fail serialization. Controller recreated once AFTER all native tests and restarted without another runner. Final author/cold acceptance both passed; earlier failed artifacts remain distinguished from final evidence.
- Final mapped reviewer approved only the two confirmed equality corrections after controller failing-first proof (bound inclusivity and channel native clock); no speculative wider re-review. Exercised source903fileSHA256:1a3c341fe67f5f5e66276395f6e4bb7fc8076726228dfc1844d03c798f4fdde7. Source manifest Saved/Issue176/verified_candidate_source_manifest.json binds native/Python/runtime evidence.
- Targeted cortex-sync-docs audit reported local://issue176-project-doc-sync-report.md; user approved both bounded workspace CortexUMG/INDEX updates. Toolkit five-file synchronization reviewed for exact native commands/guards/limits; generated fallback --from-fixture --check passed. UE5.6/5.7 remain unverified.

## Simplicity check

- [x] Think before acting — approved spec, exact existing owners, engine APIs, test conventions and generator invocation inspected before task design.
- [x] Simplicity first — two routed commands, one private native content helper, existing router/fingerprint/removal reused; one native edit owner avoids duplicate recovery systems.
- [x] Surgical changes — file ownership and non-goals bound implementation; parent gitlinks and unrelated user processes/work remain outside edits.
- [x] Goal-driven execution — task/checkpoint matrix covers every acceptance criterion, with runtime/cold-persistence gates and no passing claims before execution.

## PR178 compatibility correction

Retain @Boshchuk's three-loop correction and existing compatibility adapter. Independently compile all three baseline/candidate translation units with real UE 5.8 headers and Clang's range-loop diagnostic promoted to an error; build/link with supported MSVC, run the complete UMG suite, and exercise routed authoring plus exact nested detailed-track readback.

Completed evidence and platform limits: [PR178 verification](../verification/2026-10-13-pr178.md). Public contracts are unchanged; no toolkit or generated-schema synchronization is needed.
