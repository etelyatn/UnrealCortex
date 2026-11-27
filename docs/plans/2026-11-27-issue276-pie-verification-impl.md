# Issue 276 implementation plan

**Goal:** Deliver every item of UnrealCortex#276. Contracts are in `2026-11-27-issue276-pie-verification-design.md`.

**Architecture:** Four slices, each owning its own files. Slices A, B and C are C++ in separate modules and are
implemented in parallel without builds. The controller builds once, routes compile and test failures back to the
owning slice, and owns slice D plus integration.

**Tech:** UE 5.8 C++ automation tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`, latent PIE commands), Python MCP
(pytest).

## Global constraints

- Epic coding standard. Log categories: `LogCortexEditor`, `LogCortexQA` and `LogCortexUMG`. Never `LogTemp`.
- All UObject access happens on the game thread. Stats-thread data is marshalled to the game thread before any
  JSON or UObject work.
- Every new param is declared in `GetSupportedCommands`, because the router refuses undeclared params. Read-only
  commands join the handler's read-only set.
- Tests must produce zero warnings. Use `AddInfo` for skip conditions. Mark created assets as garbage on cleanup.
  Guard `LoadObject` with `FPackageName::DoesPackageExist`.
- PIE tests use the existing latent pattern (`FStartPIECommand(false)`, then wait until playing, then
  `FEndPlayMapCommand`, then wait until stopped).
- No new modules. Only one new dependency: CortexUMG → CortexEditor (private).

## Review focus

1. A timed hold that is replaced, stopped, or outlived by the end of PIE leaves no stale timer. A later run of the
   same action is unaffected.
2. `action_name` as a package path for an asset that exists but is not loaded. A nonexistent path produces no
   SkipPackage warning.
3. In `get_live_widgets`, a requested child that is absent is listed in `missing` and its instance is kept. A widget
   class with no live instances gives `count: 0` and is not an error.
4. `broadcast_delegate` with a wrong argument type or a missing argument leaves no half-initialized parameter memory:
   `DestroyStruct` runs on every path.
5. `capture_stats` reached by PIE end or by editor shutdown mid-capture: the stats delegate is unsubscribed and the
   pending callback completes exactly once.

## Design-review corrections

The design doc is authoritative. After the review it adds the following tests to the slices below:

- **Slice A:**
  - `capture_stats` finds a known UFunction by name. The function is invoked through `ProcessEvent` every frame by a
    test-spawned ticking actor, or the test uses an existing sandbox Blueprint.
  - `capture_stats` with `timeout_seconds: 1` and `frames: 600` completes once with `TIMEOUT` and releases the capture.
    A following capture succeeds.
  - The `set_pie_cursor` readback is checked on a later tick. The cursor is restored after PIE ends.
- **Slice B:** a sparse multicast delegate fixture whose listener removes itself during the broadcast.
  `bound_count` is reported and nothing crashes. An unbound sparse delegate gives `bound_count: 0`.
- **Slice C:**
  - A Title bound through a text delegate reports `text_source: "slate"` and the bound value.
  - A diff with only `label` re-reads using the snapshot's own selectors.
  - A count mismatch is reported in `added_instances` / `removed_instances`.
  - A truncated snapshot gives `complete: false` and `identical: false`.

## Slice A — CortexEditor (items 1, 2, 7, 8)

**Files:**

- `Source/CortexEditor/Private/Operations/CortexEditorInputOps.{h,cpp}`
- `Source/CortexEditor/Private/CortexEditorPIEState.{h,cpp}`
- `Source/CortexEditor/Private/CortexEditorCommandHandler.cpp`
- New: `Source/CortexEditor/Private/Operations/CortexEditorStatsOps.{h,cpp}`
- New: `Source/CortexEditor/Private/Operations/CortexEditorCursorOps.{h,cpp}`
- Tests:
  - `Source/CortexEditor/Private/Tests/CortexEditorInputContinuousTest.cpp`
  - new `CortexEditorStatsCaptureTest.cpp`
  - new `CortexEditorPIECursorTest.cpp`

**Tasks:**

1. **Timed continuous input.** Write the test `Cortex.Editor.Input.PIE.Continuous.TimedStartRepliesImmediately`:
   - Start with `duration_ms: 400`. The result is not deferred and `bSuccess` holds. `injecting == true`, `duration_ms == 400`.
   - After 2 world ticks, `HasContinuousInputInjectionForAction` is true and the action value is non-zero.
   - After 0.8 s, `HasContinuousInputInjectionForAction` is false, and `stop` replies `owned: false`.

   Then rewrite the existing timed tests to the new contract. Delete callback-only tests that no longer have a
   subject (`StopCompletesPendingOnce`, `PIEEndCompletesPendingOnce`, `CancellationRejectsReentrantStart`, and the
   `duration_ms requires a deferred callback` assertion). Keep the stop, replacement, disconnect and PIE-end stop
   semantics under test.

   Implement: remove `bHasCallback`/`CallbackId` from `FContinuousInputRun` and the pending-callback completion in
   `HandleContinuousInputTimerElapsed`.
2. **Action resolution.** Write the test `Cortex.Editor.Input.ResolvesActionAssetForms`. It is a non-PIE unit test
   of a newly exposed helper,
   `FCortexEditorInputOps::ResolveInputActionObject(const FString&, FString& OutError) -> UInputAction*`.
   It covers:
   - a transient action by full path and by short name;
   - a saved `UInputAction` fixture package `/Game/Test/Issue276/IA_Issue276Probe`, created and saved in the test,
     then unloaded and garbage-collected, then resolved by package path and by object path, which loads it;
   - a nonexistent `/Game/Nope/IA_X`, which returns null with no warning.

   Use the helper in `ResolveEnhancedInputTarget`. Update the `action_name` description in both command
   declarations as in the design.
3. **`editor.capture_stats`.** Write tests:
   - `Cortex.Editor.Stats.CaptureRejectsInvalidParams` (frames 0 and 601, non-array filter, negative `min_ms`);
   - `Cortex.Editor.Stats.CaptureReturnsFrames` (frames 5, no filter → `frame_count == 5`, `nodes` non-empty, `frame_time_ms.avg > 0`, completed through the deferred callback, latent wait ≤ 10 s);
   - `Cortex.Editor.Stats.CaptureFilterReturnsMatchingRoots`: `filter: ["FrameTime"]`, or a stat the engine always emits — pick it by inspection; every root's name or description contains the filter;
   - `Cortex.Editor.Stats.CaptureRejectsConcurrent`.

   Implement as in the design, `FDumpMultiple` style, owned by a `TSharedPtr` capture object. It unsubscribes in its
   destructor and on editor shutdown.
4. **`editor.set_pie_cursor`.** Write tests:
   - `Cortex.Editor.PIE.SetCursorRequiresPIE`;
   - `Cortex.Editor.PIE.SetCursorRejectsOutsideViewport`;
   - `Cortex.Editor.PIE.SetCursorUpdatesPlayerMousePosition`: set (100, 80); the reply's `player_mouse_position`
     equals it within 1 px; after 3 world ticks, `PC->GetMousePosition` still equals it within 1 px.

   The last test requires rendering and a real desktop session. Skip it with `AddInfo` when `FSlateApplication` has
   no platform cursor. Implement as in the design. Restore the OS cursor on `HandleEndPIE`, with the
   `CortexEditorSelfCursorMove` expectation, as `ReleaseHeldInputs` does.

## Slice B — CortexQA (item 6)

**Files:**

- New: `Source/CortexQA/Private/Operations/CortexQADelegateOps.{h,cpp}`
- `Source/CortexQA/Private/CortexQACommandHandler.cpp`
- Test: new `Source/CortexQA/Private/Tests/CortexQABroadcastDelegateTest.cpp`

**Fixture:** a test-only `UCLASS` component in the CortexQA test sources with:

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCortexQATestTargetChanged, AActor*, Target, int32, Code)
```

It exposes `UPROPERTY(BlueprintAssignable) OnTargetChanged`, plus a `UFUNCTION` handler that records the last args
and a call count. The component is added at runtime to a PIE-spawned actor, and the test binds the handler with
`AddDynamic`.

**Tests:**

- `Cortex.QA.BroadcastDelegate.InvokesBoundHandlers`: args `{Target: <actor path>, Code: 7}`. The handler receives
  them, the call count is 1, and `bound_count == 1`.
- `Cortex.QA.BroadcastDelegate.RejectsMissingAndUnknownArgs`: the message lists `Target:AActor*`, `Code:int32`.
- `Cortex.QA.BroadcastDelegate.RejectsNonDelegateProperty`
- `Cortex.QA.BroadcastDelegate.RequiresPIE`

**Implement** as in the design. Component lookup by `GetName()`/`GetFName()` over `Actor->GetComponents()`.

## Slice C — CortexUMG (items 4, 5)

**Files:**

- `Source/CortexUMG/CortexUMG.Build.cs` (add the private `CortexEditor` dependency)
- New: `Source/CortexUMG/Private/Operations/CortexUMGLiveWidgetOps.{h,cpp}`
- `Source/CortexUMG/Private/CortexUMGCommandHandler.cpp`
- Test: new `Source/CortexUMG/Private/Tests/CortexUMGLiveWidgetTest.cpp`

**Fixture:** a transient Widget Blueprint is not needed. In PIE, `CreateWidget<UUserWidget>` of a test-only native
`UUserWidget` subclass declared in the UMG test sources. Its `NativeOnInitialized`, or a construct-time
`WidgetTree` build, creates a `UTextBlock` named `Title` (text `"Hello"`, `bIsVariable = false`) and a `UImage`
named `Icon`. It exposes one `UPROPERTY` `int32 Score`. Add it to the viewport.

**Tests:**

- `Cortex.UMG.LiveWidgets.RequiresPIE`
- `Cortex.UMG.LiveWidgets.ReadsNamedChildren`: `text == "Hello"`, Title `visibility`, Icon `style.brush_resource`
  key present, `properties.Score`, and a missing name reported in `missing`.
- `Cortex.UMG.LiveWidgets.NoInstancesIsEmpty`
- `Cortex.UMG.LiveWidgets.SnapshotThenDiffIdentical`: `summary == "0 of N entries differ"`, N > 0.
- `Cortex.UMG.LiveWidgets.DiffReportsChangedText`: set Title to `"Bye"`; one difference, key
  `[0].widgets.Title.text`.
- `Cortex.UMG.LiveWidgets.RejectsBadLabel`: `../x`, `""`.
- `Cortex.UMG.LiveWidgets.DiffMissingSnapshot`: `FILE_NOT_FOUND`.

Tests delete their snapshot files.

**Implement** as in the design.

## Slice D — controller (item 3a, integration)

1. Item 3a: write a pytest in the existing MCP router tests, next to the zero-command test. A wrong key gives the
   new message with the received keys; an empty `commands` list keeps the old message. Then change
   `_batch_has_zero_commands` / `strict_router_tool`.
2. Build. Route compile errors to their slices. Run `Cortex.Editor.Input+`, `Cortex.Editor.Stats+`,
   `Cortex.Editor.PIE+`, `Cortex.QA+` and `Cortex.UMG+` serially.
3. Regenerate the capabilities fixture and fallback from the live editor (`MCP/scripts/sync_fallback.py`). Run the
   Python non-e2e suite.
4. Live MCP acceptance for every item, plus a Cortex MCP benchmark spot check of the changed domains.
5. Docs:
   - README;
   - CortexSandbox `docs/systems` (editor, umg, qa);
   - toolkit `qa-patterns.md` and `mcp-tool-reference.md`;
   - a verification report, `docs/verification/2026-11-27-issue276.md`.
6. Clang syntax gate on the final head. Patch version bump. PR, review, merge, close #276.
