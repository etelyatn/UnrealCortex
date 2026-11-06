# Blueprint layout collision correction

Issue: https://github.com/etelyatn/UnrealCortex/issues/164
Reporter: @etelyatn. Design approved in the current issue-handling session.

## Outcome

Readable node-body separation for full and incremental Blueprint layout; established placement is immutable in incremental mode. Layout preserves node GUIDs, links, pin defaults and behavior. Exact selected root/composite graphs are targeted. Identical options produce stable positions. Editor display and explicit save/reload agree.

## Observed baseline

Source: 439507be42c4b4f3c8cf9ac4361efd6d40042075; plugin 0.3.5; UE 5.8.3. Two connected PrintString nodes: source established at (288,384), target reset to (0,0). Incremental graph.auto_layout succeeds and places target at (288,384), directly stacking both bodies. Native readback and Editor screenshot confirmed. Requests/responses preserved locally in Saved/Issue164BaselineEvidence.json.

Native AutoLayout estimates every Blueprint node as width 200. BlueprintEditorLibrary reported stored PrintString width 250, but the factory-measured PrintString clearance assertion passed on baseline: stored geometry alone is not proof of a full-layout body overlap. A longer native call title exercises actual widget bounds. Grouped/disconnected overlap hypotheses require regression verification before attribution. Baseline repeated full formatting also dirtied a clean package.

## Design

Retain the existing execution-first layout and public CalculateLayout signature. Obtain Blueprint node-body geometry using existing engine graph-widget facilities, including unopened graphs, rather than assuming fixed widths. After final grid positioning, deterministically separate movable rectangles, reserving immutable established rectangles for incremental mode. Keep connectivity-informed candidate placement; resolve collisions without moving established nodes. Grid rounding must not erode the requested separation. Avoid leaving an eligible node at the (0,0) sentinel after placement.

Expose implemented mode/horizontal_spacing/vertical_spacing through the current graph_cmd live schema and synchronized fallback contracts. Report actual changes; repeated unchanged layout must not create a transaction or dirty the package. Persistence remains a separate explicit save operation. Invalid composite targeting must not silently format another graph.

Normal authoring placement initializes call-function metadata before layout measurement. Engine
loading otherwise adds a DevelopmentOnly banner to newly authored PrintString calls and changes
their body height on the first reload. Initialize calls after binding their function; never change
enabled state inside formatting. Check the nonzero sentinel after every collision displacement,
not only before separation, including obstacles at negative coordinates.

## Constraints

- Game-thread UObject/Slate access; no domain-to-domain dependency.
- Normal task branch in Plugins/UnrealCortex; no worktrees, reset, clean or unrelated user edits.
- All shell commands through rtk.
- Regression red before production changes; zero-warning native verification.
- No replacement formatter, implicit save, compatibility shim or unrelated refactor.

## Acceptance matrix

| Criterion | Evidence required |
|---|---|
| Separated bodies and readable flow | Rectangle assertions plus actual Blueprint Editor screenshots for execution branch and pure producers |
| Initially stacked nodes | Two-node confirmed incremental regression and full baseline |
| Full/incremental contract | Full selected graph; new-vs-established rectangle separation and exact established-position equality |
| Layout-only changes | Snapshot GUIDs, links, default representations before/after; compile fixture |
| Exact graph targeting | Root selection and composite child; sibling/root positions unchanged |
| Persistence/display | Native coordinates versus rendered bodies, explicit save, fresh reload and readback |
| Repeat stability | Exact position equality on repeated full/incremental options; unchanged outcome and clean package |
| Generic regression | Native deterministic minimal fixture for confirmed collision; no game-specific types |

## Simplicity check

- [x] Think before acting — native two-node stacking and visual reproduction established.
- [x] Simplicity first — retain current layout owner and engine geometry facilities.
- [x] Surgical changes — layout algorithm, Blueprint adapter, call placement initialization, current schema and regression coverage only.
- [x] Goal-driven execution — all issue acceptance rows proven by final321/321 Graph native tests, fresh real MCP first-save/reload invariants/stability, and actual Editor visuals; independent source review has no remaining findings.
