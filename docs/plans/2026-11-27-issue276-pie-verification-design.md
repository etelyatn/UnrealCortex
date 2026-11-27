# Issue 276 design: PIE verification gaps

**Request:** [UnrealCortex#276](https://github.com/etelyatn/UnrealCortex/issues/276), follow-up to #274. Reported on
plugin 0.4.20. Triaged against `main` 0.4.31 (`3970d1c2`) with a live CortexSandbox editor (probes under
`Saved/Issue276/`, task workspace). Delivery: every item.

## Triage

| Item | Current behavior (0.4.31, observed) | Route |
|---|---|---|
| 1 timed `inject_input_continuous` | A start with `duration_ms: 4000` holds the run: an `update` from a second connection succeeds at 0.5 s, 2 s and 3.5 s. But the command is deferred, so the only reply arrives at 4.0 s and reads `{"injecting": false}`. The caller is blocked for the whole hold, cannot observe the held state, and sees one reply that says "not injecting". Above the 30 s deferred timeout the reply is a timeout while the run continues. | Implement |
| 2 asset path for `action_name` | `IA_Jump` and `/Game/Input/Actions/IA_Jump` both return `INPUT_ACTION_NOT_FOUND`. Resolution is `FindObject` (full object path, loaded objects only) then `FindFirstObject` (loaded short name). A package path has no `.Asset` suffix, and an unloaded asset is never found. | Implement |
| 3a `batch_query` wrong key | Native refusal already names the keys (`Unknown param 'list'. Accepted params: commands, steps, …`). The MCP layer intercepts first: `_batch_has_zero_commands` treats a missing key like an empty list and returns "requires at least one command". | Implement (Python) |
| 3b `get_operation_schema` with `operation` | Already resolved: the router refuses undeclared params inside a batch as well, `Unknown param 'operation'. Accepted params: domain, command` (issue 255 contract). | Already resolved |
| 4 live widget readback | No runtime widget command exists. UMG commands read design-time trees only. | Implement |
| 5 snapshot and diff | No label-keyed snapshot exists. Blueprint/Data diffs are path-based and module-private. | Implement |
| 6 delegate broadcast | No code calls `ProcessEvent` or broadcasts a multicast delegate. | Implement |
| 7 per-function cost capture | `execute_console_command` now reaches `stat` (`route: engine_exec`, #274). But `stat dumpave` writes to the log from the stats thread and has no end marker. | Implement |
| 8 PIE cursor override | `inject_mouse` move is a Slate event. `APlayerController::GetMousePosition` reads `FSceneViewport::CachedCursorPos`, and Slate's per-tick synthetic cursor move re-samples the real OS cursor, so the injected position is lost within one or two ticks. #274 only documented this. | Implement |
| 9 floating-PIE screenshot | Already resolved by #274 item 7: `start_pie mode:new_window` then `capture_screenshot` returns `source: pie`, `view: pie_game_camera`, `window_title: "CortexSandbox Preview …"`, and the image is the game view (`Saved/Issue276/item9_newwindow.png`). | Already resolved |

## Contracts

### 1. Timed continuous input returns immediately

`editor.inject_input_continuous` with `mode: start` and `duration_ms` is no longer deferred. It starts the run,
arms the stop timer and replies at once:
`{action_name, mode: "start", injecting: true, duration_ms}`. When the timer fires, the run stops exactly as an
explicit `stop` would. A `stop` before expiry replies `owned: true`; after expiry it replies `owned: false`.
Replacement, PIE end and TCP disconnect keep their current stop semantics. Since no caller is pending any more, the
continuous-input pending-callback path (`bHasCallback`, `CallbackId`, the "requires a deferred response callback"
refusal) is removed. `inject_input_sequence` keeps its deferred contract.

Ruling: the issue's stated expectation is an immediate `injecting: true` while the action is held. A blocking reply
cannot meet it. Blocking also prevents observing the held state and fails for durations above the deferred timeout.
Callers who need to wait for the end can use `qa.wait_for`, or call `stop`.

### 2. `action_name` accepts every form of one asset

Resolution order, shared by `inject_input_action` and `inject_input_continuous`:

1. Full object path, or a loaded short name (current behavior).
2. A long package path without an object name (`/Game/Input/Actions/IA_Jump`): becomes `Package.AssetName`.
3. A full path to an asset that is not loaded: loaded with `LoadObject`, guarded by `FPackageName::DoesPackageExist`
   (no SkipPackage warnings).

`INPUT_ACTION_NOT_FOUND` messages list the accepted forms. Schema text: "Input action: object path
(`/Game/Input/IA_Jump.IA_Jump`), package path (`/Game/Input/IA_Jump`), or the name of a loaded action".

### 3a. Batch container key

When `params` has neither `commands` nor `steps`, the MCP refusal says so and names the received keys: "batch_query
requires params.commands (array of {command, params}); 'steps' is also accepted. Received keys: [list]". An explicit
empty list keeps the zero-command message.

### 4. `umg.get_live_widgets` (read-only, CortexUMG)

Params:

- `widget_class` (required): Widget Blueprint asset path, generated class path, or native class name.
- `widgets` (optional string array): child widget names. Default is every named widget in the instance tree.
- `include_properties` (optional bool, default `true`).
- `max_instances` (optional int, default 10, maximum 100).

Behavior: requires PIE (`PIE_NOT_ACTIVE`). Lists every `UUserWidget` in the PIE world whose class `IsChildOf` the
resolved class, including widgets that are not in the viewport. `GetUniqueID` is a reusable slot, not a creation
order, so instances are ordered by FName number (the per-class creation counter) and then by name. Children are
resolved with `UUserWidget::WidgetTree->FindWidget(Name)`, a name lookup that does not depend on `bIsVariable`, so it
also finds bound widgets that Blueprint cannot see. A requested name that is missing is listed in that instance's
`missing`, and the instance stays in the result.

Per instance:

```
{index, name, class, in_viewport, visibility,
 properties: {...},
 widgets: {Name: {class, visibility, is_enabled, render_opacity, text?, style?}},
 missing: [...]}
```

- `properties` holds the reflected properties declared below `UUserWidget`, minus widget references and delegates, through `FCortexSerializer::PropertyToJson`.
- `text`: `UTextBlock`, `URichTextBlock`, `UEditableText`, `UEditableTextBox`. Comes from `GetText()`, with
  `text_source: "slate"` when the Slate widget exists (the effective value, bindings included) and `"property"`
  otherwise.
- `bindings`: the names of bound property delegates, for example `ColorAndOpacity`. Style values are stored values,
  so a bound style is reported as bound rather than evaluated.
- `style`:
  - text widgets: `color`, `font_object`, `typeface`, `size`.
  - `UImage`: `brush_resource` (object path), `image_size`, `tint`.
  - `UButton`: `normal`, `hovered`, `pressed` brush resources.
  - `UBorder`: `brush_resource`.

Response: `count`, `instances`, `truncated`.

CortexUMG gains a private dependency on `CortexEditor` (shared infrastructure, used for `FCortexPIEUtils`). QA and
Level have the same dependency.

### 5. `umg.snapshot_live_widgets` and `umg.diff_live_widgets`

`snapshot_live_widgets` takes the same params as `get_live_widgets`, plus a required `label`. `diff_live_widgets` takes
only `label`. The label must match `[A-Za-z0-9_.-]{1,64}`; any other value → `INVALID_FIELD`.

- **Snapshot** writes the readback to `Saved/Cortex/WidgetSnapshots/<label>.json` through `FCortexSafeFileContract`.
  The file holds `format_version: 1`, the capture selectors (`widget_class`, `widgets`, `include_properties`,
  `max_instances`), `captured_at`, `truncated` and `instances`. An existing label is overwritten. Reply:
  `{label, path, instance_count, entry_count, truncated}`.
- **Diff** reads the snapshot (`FILE_NOT_FOUND` if it is absent, `INVALID_FIELD` on a format mismatch), then takes a
  live readback with the snapshot's own selectors and compares flattened value entries.
  - Identity metadata (`name`, `index`) is not compared. An entry key is `[<index>].<field path>`, for example
    `[0].widgets.Title.text`.
  - Instances are paired by their order (see above). When the counts differ, the extra instances are reported as
    `added_instances` / `removed_instances`, and their entries count as differing.
  - Reply:

    ```
    {label, total_entries, differing_entries, identical, complete,
     summary: "<d> of <n> entries differ",
     differences: [{key, before, after}],
     added_instances, removed_instances}
    ```

  - `complete` is false when either readback was truncated, and `identical` requires `complete`.
  - `differences` is capped at 200 entries, with `differences_truncated: true` when the cap is hit.

### 6. `qa.broadcast_delegate` (CortexQA)

Params:

- `actor` (required): PIE actor, resolved as in `qa.set_actor_property`.
- `component` (optional): component name on that actor.
- `delegate` (required): property name.
- `args` (optional object): signature parameters by name.

Behavior: requires PIE. The property must be a multicast delegate (inline or sparse). Any other property type is
refused with `INVALID_FIELD`, naming the property's type. Steps:

1. Allocate the signature's parameter struct with aligned `GetStructureSize()` storage and `InitializeStruct` it.
   A scope guard runs `DestroyStruct` on every exit path.
2. Import each argument with `FCortexSerializer::JsonToProperty`.
3. Broadcast with `ProcessDelegate<UObject>` (the UE 5.8 compat name).
   - Sparse delegates: hold `GetSharedMulticastDelegate` across the dispatch, as the engine's sparse `Broadcast` does.
   - An unbound sparse delegate has 0 listeners.

Every signature parameter is required. Unknown or missing args → `INVALID_FIELD`, listing the expected
`name:type` pairs. Out/return parameters are refused.

Reply: `{actor, component, delegate, signature, bound_count, args}`. `bound_count` is the invocation-list size
before the broadcast.

### 7. `editor.capture_stats` (deferred, CortexEditor)

Params:

- `frames` (optional int, 1–600, default 60).
- `filter` (optional string array): case-insensitive substrings matched against the stat's short name and its
  description.
- `max_depth` (optional int, default 6).
- `min_ms` (optional number, default 0).
- `timeout_seconds` (optional number, 1–300, default 30). It is the capture's own deadline and also sets
  `DeferredTimeoutSeconds`, so the TCP reply outlives it. The MCP router widens its receive window to
  `timeout_seconds` + 30 s, as it does for `anim.sample_runtime_pose`.

Implementation follows `FDumpMultiple` (engine `StatsCommand.cpp`) without parsing the log:

- All stats-state work runs on the stats pipe, as the engine's own `stat` commands do: subscribing to
  `NewFrameDelegate`, aggregation, unsubscribing, and the balanced `FThreadStats::PrimaryEnableAdd` /
  `PrimaryEnableSubtract`. `GetLocalState()` is stats-thread-only.
- For each frame, build a fresh root with `UncondenseStackStats`, then `MergeAdd` it, and record the game-thread frame
  time.
- After N frames, `Divide(N)`. Only plain copied results (names, numbers) cross to the game thread.
- Completion is exactly once: success, deadline (`TIMEOUT`, capture released), PIE-independent editor shutdown, or
  module shutdown, which drains the pipe work.
- The command does not toggle global stat settings (`stat namedevents`, `bp.VerboseStats`). Reflected/Blueprint
  scopes are compiled into Development builds, and the `UObjects` group is on by default.
- An incompatible raw-stats mode (`stat startfile`, `-statsfile`) → `INVALID_OPERATION`.

Reply: `{frame_count, first_frame, last_frame, frame_time_ms: {avg, max}, nodes, truncated}`.

- `nodes` are trees of `{name, description, inclusive_ms, exclusive_ms, calls_per_frame, children}`.
  - `calls_per_frame` is fractional (total calls / frames), because the engine's integer `Divide` rounds a call that
    happens once per capture down to 0.
- With `filter`, `nodes` are the matching subtrees (roots are the matching nodes). A known Blueprint/UFunction is
  found by its asset or function name.
- Without `filter`, `nodes` are the thread roots down to `max_depth`.
- Refusals: `INVALID_OPERATION` when `STATS` is compiled out, or when a capture is already running. Other refusals follow the usual field validation.
- PIE is not required.

### 8. `editor.set_pie_cursor` (CortexEditor)

Params: `x`, `y` (required numbers, PIE viewport pixel coordinates).

Behavior:

- Requires PIE (`PIE_NOT_ACTIVE`). The scene viewport is resolved from the first local player's
  `ViewportClient->GetGameViewport()`, the same viewport `GetMousePosition` reads, in both PIE modes. A position
  outside `GetSizeXY()` → `INVALID_FIELD`.
- Refused with `INVALID_OPERATION` while the viewport holds hidden-cursor mouse capture. In that state the engine
  warps the cursor back on the next move, so the override would not persist.
- Calls `FSceneViewport::SetMouse(X, Y)` with viewport pixels, without pre-scaling for DPI. It sets `CachedCursorPos`
  and moves the OS cursor through `FSlateApplication::SetCursorPos`, so Slate's re-sampling keeps the position.
- On the first call in a PIE session, it records the OS cursor. It restores the cursor after PIE teardown, one tick
  after `EndPIE`, once viewport capture and clipping are released, and only if the cursor is still where Cortex put it.

Reply: `{x, y, viewport_size, player_mouse_position}`. `player_mouse_position` is read back from the same local
player's `GetMousePosition`. Tests check the position again on a later tick.

Documented: the real cursor moves, as during absolute Replay; the override lasts until the real mouse moves. Desktop
`SendInput` is not used, so the desktop-idle guards are unaffected.

## Shared contracts and generated artifacts

- Every new param or command is declared in `GetSupportedCommands`. New read-only commands are added to their
  handler's read-only set.
- `MCP/tests/fixtures/capabilities_cache_full.json` and `_fallback_generated.py` are regenerated once, at integration.
- Docs: README, `CortexSandbox/docs/systems/{cortex-editor,cortex-umg,cortex-qa}.md`, cortex-toolkit
  `qa-patterns.md` and `mcp-tool-reference.md`.
