# Issue 268 design: `editor.capture_editor_panel`

**Request:** [UnrealCortex#268](https://github.com/etelyatn/UnrealCortex/issues/268). `editor.capture_screenshot`
only reads the level viewport or the PIE game view. Documentation for artists needs pictures of Editor UI: a
material instance's parameter list, a data asset's Details panel, a material graph. On macOS a shell
`screencapture` needs Screen Recording permission (it returns only the wallpaper), so there is no fallback.

## Decision

Add one command, `editor.capture_editor_panel`, that renders a Slate widget of the Editor to a PNG with
`FSlateApplication::TakeScreenshot`. The engine draws the widget's window and reads the requested rectangle
back from that window's own back buffer, so the capture shows Editor UI even when another application covers
the window, and needs no OS screen-capture permission. The Widget Reflector snapshot, `UAutomationBlueprintFunctionLibrary`
and `FGameViewportClient` screenshots use the same call.

`capture_screenshot` is unchanged: it renders the 3D viewport with camera provenance; this command captures UI.

## Contract

| Param | Type | Meaning |
|---|---|---|
| `asset_path` | string, optional | Asset whose editor is opened (or focused) with `UAssetEditorSubsystem::OpenEditorForAsset`. Without it, the root is the active top-level regular window, or the main frame window when the Editor is not the foreground application. |
| `panel` | string, optional | `editor` (default with `asset_path`): the asset editor's tab content. `window` (default without `asset_path`): the top-level window holding the root. `details`: an `SDetailsView` under the root. `graph`: an `SGraphEditor` under the root. |
| `output_path` | string, optional | PNG path; default `Saved/CortexScreenshots/cortex_panel_<timestamp>.png`, as `capture_screenshot`. |

`StrictParams`: unknown parameters are refused by the router.

When several `details`/`graph` widgets are visible (the material editor shows a Details panel and the preview
scene's settings), the one with the largest on-screen area is captured, and `match_count` reports how many
matched. Only painted widgets count: background tabs are not in the live widget tree. `details` matches
`SDetailsView` only (asset editors such as the material instance and data asset editors, and the level editor);
the DataTable row editor (`SStructureDetailsView`) is outside the request and is not matched.

Response: `path` (absolute), `width`, `height` (PNG pixels), `file_size_bytes`, `capture_time_ms`, `panel`,
`widget_type` (`SWindow`, `SDetailsView`, `SGraphEditor`, or the editor content's type), `window_title`,
`match_count` (`details`/`graph` only), and with `asset_path` also `asset_path` and `editor_name`
(`IAssetEditorInstance::GetEditorName`).

| Condition | Code |
|---|---|
| `panel` not one of the four names; `panel: editor` without `asset_path` | `INVALID_FIELD` |
| Asset path does not resolve to a loaded or on-disk asset | `ASSET_NOT_FOUND` |
| Asset is a map (opening it would switch the editor world) | `INVALID_OPERATION` |
| No asset editor opened, or it has no Slate tab | `EDITOR_NOT_AVAILABLE` |
| No Slate application, or the process cannot render (`-NullRHI`) | `EDITOR_NOT_AVAILABLE` |
| A modal window other than the target window is open (Slate then draws only the modal) | `EDITOR_BUSY` |
| Target window hidden or minimized | `SCREENSHOT_FAILED` |
| No visible widget of the requested panel kind | `WIDGET_NOT_FOUND` |
| Readback, encode or write failure | `SCREENSHOT_FAILED` |

No refusal writes a file.

## Mechanics and safety

- Asset resolution and map refusal reuse the `wait_ready` path (`FCortexEditorTurnaroundOps::OpenAssetInEditor`,
  extracted from `ReopenAssets`): normalize the path, `FindObject`, then `LoadObject` only after
  `DoesPackageExist`; maps refused before any load.
- The editor root is `IAssetEditorInstance::GetAssociatedTabManager()->GetOwnerTab()->GetContent()`.
- One `FSlateApplication::Tick` runs before the widget search so a newly opened editor has arranged geometry.
- Graph views settle over real time: a newly opened graph editor jumps its view to a node (the material editor
  centers its result node) only once the node widgets exist, then scrolls with an active timer driven by frame
  delta time. A capture taken in the opening call showed the pre-jump view (54-57% of pixels differed from a
  later capture). Before reading back, capture ticks Slate at ~60 Hz, at most 2 s, while any `SGraphEditor`
  under the target has `SNodePanel::HasDeferredZoomDestination()` or an active timer on its graph panel
  (`SGraphEditor::GetGraphPanel()`; the panel is not reachable through `GetChildren`). This also covers a view
  still moving after `focus_node`. CortexEditor gains a private `GraphEditor` module dependency for it.
  A whole-subtree active-timer wait was rejected: asset editors keep perpetual timers, so it always ran to the
  budget.
- Candidates must have a widget path (`GeneratePathToWidgetUnchecked`); `TakeScreenshot` calls the checked
  variant, which asserts when the widget is not in a window.
- The window must be drawn by `TakeScreenshot`'s `PrivateDrawWindows`; otherwise `FSlateRHIRenderer` keeps the
  pending screenshot pointer to our pixel buffer for a later frame. Hidden/minimized windows and foreign modal
  windows are refused before the call. If readback still produced no pixels, the pending request is replaced by
  one targeting no window (`PrepareToTakeScreenshot` with a null window), which no frame matches.
- Alpha is forced to 255 as in `capture_screenshot`.

## Rejected alternatives

- `FWidgetRenderer` off-screen render: repaints a widget that is already parented in a live window into a
  second render target; invalidation/retainer panels and docked tab content do not reliably repaint outside
  their window, and the result can diverge from what the user sees.
- OS screen capture: needs Screen Recording permission on macOS (the issue's failure) and captures occluding
  windows.
- Per-editor tab IDs for `panel` (`MaterialEditor_Properties`, ...): differs per editor class; widget type search
  is uniform across editors and also covers the level editor's actor Details panel.

## Acceptance

1. Material editor: `graph` → `SGraphEditor`, `details` → `SDetailsView`, `editor` default, `window`; each PNG
   decodes to the reported size and is not blank; panel ⊂ editor ⊂ window by size; the graph captured in the
   call that opens the editor matches a later capture (< 2% of pixels differ).
2. No asset: default captures a window; `details` captures the level editor's Details panel with an actor
   selected (selection restored). With nothing selected that `SDetailsView` is one flat colour; the
   "Select an object" hint is drawn by a sibling widget.
3. Refusals: unknown panel, `editor` without asset, missing asset, map (editor world unchanged), panel absent
   (curve editor has no graph); no file written.
4. Live MCP: `editor_cmd capture_editor_panel` on `/Game/Materials/MI_PulsatingBlue` (`details`) and
   `/Game/Materials/M_PulsatingBlue` (`graph`) produce PNGs showing the parameter list and the graph.
