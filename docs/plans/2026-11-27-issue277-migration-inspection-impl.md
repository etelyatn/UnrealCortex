# Issue 277 implementation plan

**Goal:** deliver every item of [UnrealCortex#277](https://github.com/etelyatn/UnrealCortex/issues/277) per
`2026-11-27-issue277-migration-inspection-design.md` (the spec; read it first).

**Execution:** three parallel slices with exclusive module ownership on branch `fix/277-migration-inspection`, in
two TDD phases: tests only (controller builds once and records RED), then implementation (controller builds and
records GREEN). Then capabilities fixture/fallback regeneration, Python suite, Clang gate, live editor smoke,
review, docs, version bump, toolkit sync, PR, merge.

## Global constraints

- UE 5.8, Epic coding standard. Game thread only. `FScopedTransaction` on writes; nodes touched by a write get
  `RF_Transactional` before `Modify()` so one undo restores the whole command.
- New params/commands declared in `GetSupportedCommands`; the router rejects undeclared params.
- Tests drive the public command route (handler/router JSON). New test paths are siblings, never children of an
  existing leaf test. Helpers are unity-build safe (no file-scope `using namespace`; unique helper names).
- No builds or test runs by implementers. The controller owns fixtures, docs, version files and commits.

## Slices and file ownership

| Slice | Items | Owns |
|---|---|---|
| Blueprint | 1 2 4 5 6 8 | `Source/CortexBlueprint/**` |
| Graph | 3 9 10 11 | `Source/CortexGraph/**` |
| Reflect | 7 | `Source/CortexReflect/**` |

## Tests (RED on `main` 3970d1c2, GREEN on the branch)

| Item | Automation test |
|---|---|
| 1 | `Cortex.Blueprint.ClassSettings.RemoveInterface.IslandRemoval`, updated `.PreserveImplementations` |
| 2 | `Cortex.Blueprint.RemoveGraph.Apply.HashOnly`, `Cortex.Blueprint.ClassSettings.RemoveInterface.HashOnlyApply`, `Cortex.Blueprint.ReplaceInterface.HashOnlyApply` |
| 3 | `Cortex.Graph.Issue277.AuthoringContext.GraphName`, `.AmbiguousGraphName`, updated `Cortex.Graph.Authoring.Context.FailingCases` |
| 4 | `Cortex.Blueprint.Search.VariableNodes` |
| 5 | `Cortex.Blueprint.FindVariableReferences.Basic`, `.NativeProperty`, `.Errors` |
| 6 | `Cortex.Blueprint.GetInfo.Interfaces` |
| 7 | `Cortex.Reflect.FindInterfaceReferencers`, `.FindInterfaceImplementerNarrowing`, `.FindInterfaceReferencersNative` |
| 8 | `Cortex.Blueprint.GraphMetrics.TotalsAllNodes`, `.AssetPathsBatch` |
| 9 | `Cortex.Graph.Issue277.AddNodePlacement.Sides`, `.Refusals`, `Cortex.Graph.Issue277.Schema.Declarations` |
| 10 | `Cortex.Graph.Issue277.SplitGetter.SelfMember`, `.ExternalTarget`, `.Refusals`, `.BlockedAsset` |
| 11 | `Cortex.Graph.Issue277.RemoveNodes.OverrideEvents`, `.OverrideEventsScope` |

## Integration (controller)

1. RED build and run of the table above; GREEN build; affected namespaces `Cortex.Blueprint+`, `Cortex.Graph+`,
   `Cortex.Reflect+`, `Cortex.Core+` (capabilities snapshot).
2. Regenerate `MCP/tests/fixtures/capabilities_cache_full.json` from the live editor and `_fallback_generated.py`
   (`uv run python scripts/sync_fallback.py --from-fixture`); Python suite (non-e2e).
3. Clang syntax gate; live editor smoke of each item through MCP.
4. Review, system docs (`CortexSandbox/docs/systems/`), patch version bump, toolkit surface sync, PR, merge.
