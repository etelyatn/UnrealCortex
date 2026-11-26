# Issue 268 implementation plan: `editor.capture_editor_panel`

**Goal:** capture Editor panels (asset editor, Details panel, graph, top-level window) to PNG without OS
screen-capture permission.

**Design:** `2026-11-26-issue268-capture-editor-panel-design.md` (contract, refusals, settle mechanics, rejected
alternatives).

**Base:** UnrealCortex `2f84af23` (0.4.27), branch `feat/268-capture-editor-panel`. Executed inline, TDD; one
reviewer over the branch.

## Global constraints

- CortexEditor only; new private engine dependency `GraphEditor` (no domain coupling).
- Game thread only; `LogCortexEditor`; zero warnings in `Cortex.Editor+` (rendering suite, no `-NullRHI`).

## Task 1: shared asset opening

- Extract `FCortexEditorTurnaroundOps::OpenAssetInEditor(Path, OutErrorCode, OutFailure)` from the `wait_ready`
  reopen loop (path normalization, `FindObject`, map refusal before load, `DoesPackageExist` before `LoadObject`,
  `OpenEditorForAsset`); `ReopenAssets` calls it. Codes: `ASSET_NOT_FOUND`, `INVALID_OPERATION` (map),
  `EDITOR_NOT_AVAILABLE`.
- Proof: `Cortex.Editor.WaitReady` unchanged and passing.

## Task 2: capture command (`Private/Operations/CortexEditorViewportOps.{h,cpp}`)

- `FCortexEditorViewportOps::CaptureEditorPanel(Params)`; route and `FCortexCommandInfo` (`asset_path`, `panel`,
  `output_path`, `StrictParams`) in `CortexEditorCommandHandler.cpp`.
- Shared with `capture_screenshot`: `ResolveCaptureOutputPath`, `WriteOpaquePng`.
- Widget search: `CollectWidgetsOfType` over `GetChildren`; candidates need a widget path
  (`GeneratePathToWidgetUnchecked`) and the largest draw area wins.
- Graph settle: `HasMovingGraphPanel` via `SGraphEditor::GetGraphPanel()`, ticked at 60 Hz for at most 2 s.
- Readback: `FSlateApplication::TakeScreenshot`; foreign modal → `EDITOR_BUSY`; hidden/minimized →
  `SCREENSHOT_FAILED`; unconsumed request cleared with `PrepareToTakeScreenshot(FIntRect(), sink, nullptr)`.

Tests (`Private/Tests/CortexEditorPanelCaptureTest.cpp`):

- `Cortex.Editor.CaptureEditorPanel.AssetEditorPanels`: resident material fixture; `graph`, `graph` again,
  `details`, default `editor`, `window`; PNG decodes to the reported size, not blank, file size matches; widget
  types; panel ⊂ editor ⊂ window; first graph capture differs from the second in < 2% of pixels.
- `Cortex.Editor.CaptureEditorPanel.ActiveWindow`: default window capture; `details` with an editor-world actor
  selected (selection restored).
- `Cortex.Editor.CaptureEditorPanel.Refusals`: unknown panel, `editor` without asset, missing asset, map (editor
  world unchanged), curve editor `graph` → `WIDGET_NOT_FOUND`; no file written.

## Task 3: MCP surface, docs, release

- Capabilities fixture entry from the live editor and regenerated `_fallback_generated.py`
  (`scripts/sync_fallback.py --from-fixture`).
- `docs/systems/cortex-editor.md` (CortexSandbox), verification report `docs/verification/2026-11-26-issue268.md`,
  live MCP smoke on `/Game/Materials/MI_PulsatingBlue` (`details`) and `/Game/Materials/M_PulsatingBlue` (`graph`),
  toolkit reference and surface fixture, patch bump to 0.4.28.
