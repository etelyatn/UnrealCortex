# Blueprint layout correction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use executing-plans for native execution. Steps use checkbox syntax for tracking.

**Goal:** Deliver all issue #164 acceptance criteria, not just coordinate inequality.

**Architecture:** Keep FCortexGraphLayoutOps::CalculateLayout(Nodes, Config, ExistingPositions) as the shared position owner. Blueprint AutoLayout supplies actual node geometry and applies only changed coordinates. Deterministic rectangle separation runs after grid positioning and respects immutable incremental obstacles.

**Tech Stack:** UE 5.8.3, Unreal C++, Slate graph widgets, Python MCP schema generation, native TCP smoke.

**Spec:** docs/plans/2026-10-05-blueprint-layout-design.md

## Global Constraints

- Game-thread UObject/Slate access; no domain-to-domain dependency.
- Normal task branch in Plugins/UnrealCortex; no worktrees, reset, clean or unrelated user edits.
- All shell commands through rtk.
- Regression red before production changes; zero-warning native verification.
- No replacement formatter, implicit save, compatibility shim or unrelated refactor.

## Review Focus

1. Incremental obstacle collisions and (0,0) sentinel stability.
2. Group-expanded rectangles, tall producers and disconnected components.
3. Unopened graph measurement versus actual rendered body geometry.
4. Exact child graph targeting; no root/sibling mutation.
5. Unchanged layout must not dirty a saved package; explicit persistence.

### Task 1: Shared collision handling and Blueprint adapter

**Files:** Modify Source/CortexGraph/Private/Operations/CortexGraphLayoutOps.cpp; Source/CortexGraph/Private/Operations/CortexGraphNodeOps.cpp. Add behavioral coverage beside existing Source/CortexGraph/Private/Tests/CortexGraphLayoutSubgraphTest.cpp and CortexGraphAutoLayoutTest.cpp. Inspect engine graph geometry owner before selecting the measurement API; keep engine-private details out of public headers.

**Interfaces:** Consume FCortexLayoutNode.Width/Height and ExistingPositions; return existing FCortexLayoutResult.Positions/LayerAssignment. No exported signature change.

- [x] Write deterministic incremental regression before implementation:

```cpp
FCortexLayoutNode A; A.Id = TEXT("A"); A.bIsExecNode = true; A.Width = 250; A.Height = 206; A.ExecOutputs.Add(TEXT("B"));
FCortexLayoutNode B; B.Id = TEXT("B"); B.bIsExecNode = true; B.Width = 250; B.Height = 206;
FCortexLayoutConfig Config; Config.Mode = ECortexLayoutMode::Incremental;
const TArray<FCortexLayoutNode> Nodes = {A, B};
FCortexLayoutConfig FullConfig;
const FCortexLayoutResult Full = FCortexGraphLayoutOps::CalculateLayout(Nodes, FullConfig);
TMap<FString, FIntPoint> Existing; Existing.Add(A.Id, Full.Positions[B.Id]); Existing.Add(B.Id, FIntPoint::ZeroValue);
const FCortexLayoutResult Actual = FCortexGraphLayoutOps::CalculateLayout(Nodes, Config, Existing);
TestFalse(TEXT("Established A is not moved"), Actual.Positions.Contains(A.Id));
const FIntPoint New = Actual.Positions[B.Id]; const FIntPoint Fixed = Existing[A.Id];
TestTrue(TEXT("New body has requested separation"), New.X >= Fixed.X + A.Width + Config.HorizontalSpacing || Fixed.X >= New.X + B.Width + Config.HorizontalSpacing || New.Y >= Fixed.Y + A.Height + Config.VerticalSpacing || Fixed.Y >= New.Y + B.Height + Config.VerticalSpacing);
```

- [x] Build then run `Cortex.Graph.Layout+` via cli/Testing/RunTests.ps1. Record failing assertion on unchanged production source.
- [x] Add native adapter regression with real PrintString bodies; snapshot GUIDs/pin defaults/links, exact root/composite selection, stable repeat and clean package. Use measured body bounds for separation rather than guessed constants. Add grouped multi-lane/disconnected collision cases and sentinel stability to engine coverage. Actual mixed branch/pure-producer flow is exercised through live MCP.
- [x] Implement deterministic final separation. Reserve established rectangles first in incremental mode; process movable IDs in stable order, retaining candidate X and advancing grid-aligned Y beyond intersecting rectangles until separated. Account for both dimensions and requested spacing; avoid (0,0). Do not mutate the input graph or existing positions.
- [x] Use engine graph widget measurement in Blueprint conversion. Apply only differing positions, create transaction lazily, refresh/mark modified only changed graphs. Response retains node_count/graphs_processed and adds truthful changed_node_count/unchanged. Reject composite path without a root graph rather than ignoring it.
- [x] Build; run Cortex.Graph+ and Cortex.Blueprint.Layout+ serially. Final rendering `Cortex+` is1701/1701, zero raw warnings. Fresh real MCP branch/pure/disconnected/composite and incremental fixtures preserve exact non-position/established snapshots; first explicit save/reload then full/incremental repeat is unchanged/clean. Actual final Editor full/incremental screenshots preserved.

### Task 2: Discoverable contract and delivery

**Files:** Source/CortexGraph/Private/CortexGraphCommandHandler.cpp; existing generated MCP schemas/capabilities and toolkit domain docs as required by cortex-sync-toolkit. Update this design/implementation record and plugin docs/verification/2026-10-05-issue164.md. Parent gitlinks excluded.

**Interfaces:** Current graph_cmd continues forwarding params. Native auto_layout documents mode, horizontal_spacing and vertical_spacing; no legacy standalone tool revival.

- [x] Declare already-supported options using adjacent native command parameter patterns. Update generated/fallback surface with authoritative tooling, not duplicated routers. Verify live schema returns these options and caller can invoke incremental/spacing through MCP.
- [x] Run applicable Python checks and actual routed smoke. Final Python stages:1205 passed/2 existing fixture skips; benchmark12 passed/1 no-reference fixture skip with actual visuals. Toolkit PR64 and CortexSandbox docs-only PR115 publish approved synchronization; parent gitlinks/content/cdb excluded.
- [x] Independent whole-candidate review against the acceptance matrix; fix supported consequential findings and refresh affected evidence. Round3 has no remaining findings; all corrected native tests and fresh real MCP first-reload proof pass.
- [x] Commit scoped candidate normally; publish PR linked to #164. PR165 head308831de8aadd4511f82049c87d7ac76019d1e93 merged with full expected-head protection as1bc54a240160c679feedced6b034a35e260e7766; issue164 closed only after all acceptance rows passed. ToolkitPR64 and CortexSandboxPR115 also merged.
- [x] Fast-forward affected default branches only safely; normal branches, no reset/clean/stash/worktrees. All three defaults fast-forwarded to confirmed merges; pre-existing parent gitlinks/cdb and observed content/map dirtiness preserved. Post-merge Source/MCP diff is empty, source/DLL SHA256s match tested identity, and actual root/child/incremental layouts plus real stdio MCP report unchanged.

## Plan self-review

Both tasks preserve CalculateLayout's current signature. Task 2 consumes Task 1's additive response and existing native command schema; no competing owner or shared implementation writer. Each acceptance row maps to Task 1 runtime/regression proof or Task 2 contract/publication proof. No implicit save, contributor branch deletion, warning suppression or scope reduction is authorized.
