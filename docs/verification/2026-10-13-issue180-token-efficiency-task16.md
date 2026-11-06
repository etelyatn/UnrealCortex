# Issue 180 — Token efficiency, Task 16: same-turn context dedupe and one current access policy

**Date:** 2026-10-14
**Repository:** `Plugins/UnrealCortex` (plugin repo)
**Branch:** `feat/issue-180-token-efficiency`
**Owner:** Task 16 implementer (`ImplTask16Frontend`) — RED test staging, the minimal production
change and this record. All builds/test runs, the rendering-editor RED/GREEN observation and the
registered live smoke are controller-owned.
**Status:** implementation complete; rendering-editor GREEN not yet observed by the controller.

## Scope

Two consumer-visible duplicate statements on the same send are removed:

1. The frontend sent the same auto editor-context preamble twice when an explicit `editorContext`
   provider chip (or `@editorContext` mention) was attached together with auto-context, re-gathering
   the whole editor snapshot for each occurrence.
2. The Codex default instruction block stated `Current access mode` once in the prose sentence and
   again in the appended policy block, for a total of two statements per default turn.

No new context cache/service, provider lifecycle or game state API. Game-Thread-only UObject access
is preserved; no global GC, no implicit retries and no scope reduction.

## Production contract implemented

* `SCortexInputArea::ResolveAndSend` gathers the editor snapshot **at most once per send** and only
  when something needs it (auto-context injection, or an explicit `editorContext` chip/@mention). It
  is a lazy closure over a stack `FCortexEditorContextSnapshot` + `bool`, so an unused send allocates
  and copies nothing.
* The single snapshot is passed through the provider-chip resolution
  (`ResolveProviderChip(Label, const FCortexEditorContextSnapshot* Snapshot)`). `editorContext`
  formats the supplied snapshot instead of re-gathering.
* A successful explicit `editorContext` resolution sets `bEditorContextInjected`, which suppresses
  the duplicate auto preamble for that send (`... && !bEditorContextInjected`). Auto-context without
  an explicit chip, and disabled/empty/failed providers, behave exactly as before.
* There is **no cross-turn cache**: the snapshot is a local of `ResolveAndSend`, so a later send reads
  the current selection.
* `FCortexCodexCliProvider::BuildCodexInstructionBlock`: the default (no `SystemPrompt`) prose no
  longer restates the access mode. The appended policy block
  (`Current access mode:` / `Workflow mode:` / `Project context:` / `Auto-context:`) remains the single
  statement for default, custom and resumed turns. The custom `SystemPrompt` branch is untouched.

## Changed files

| File | Change |
|---|---|
| `Source/CortexFrontend/Private/Widgets/SCortexInputArea.h` | Forward-declare `struct FCortexEditorContextSnapshot;`; `ResolveProviderChip` gains a `const FCortexEditorContextSnapshot* Snapshot` parameter. |
| `Source/CortexFrontend/Private/Widgets/SCortexInputArea.cpp` | Lazy per-send snapshot + `bEditorContextInjected`; snapshot-aware provider call sites; auto preamble suppression; `ResolveProviderChip` uses the supplied snapshot. |
| `Source/CortexFrontend/Private/Providers/CortexCodexCliProvider.cpp` | Removed the duplicate default `Current access mode: %s.` prose sentence. |
| `Source/CortexFrontend/Private/Tests/SCortexInputAreaTest.cpp` | `FTask16ActorFixture::Spawn` assigns an explicit distinct actor label (fixture defect repair). |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task16.md` | This record. |

Test files staged at RED (unchanged by this implementation): `SCortexInputAreaTest.cpp` (4 tests +
fixture) and `CortexCliSessionTest.cpp` (3 tests); `Private/Session/CortexCliSession.h` carries the
three test-only `friend class` declarations.

## Fixture correction (not a product change)

`FTask16ActorFixture::Spawn` sets `SpawnParams.Name`, which only assigns the object name. In the
editor `AActor::GetActorLabel()` defaults to the class label (`GetDefaultActorLabel()` → `Actor`) for
every instance, so the two actors in `NextTurnUsesCurrentSelection` had identical identities and the
`Task actors have distinct identities` assertion failed on a fixture reason. `Spawn` now calls
`Actor->SetActorLabel(FString(Name), false)` under `#if WITH_EDITOR`, giving each owned actor a
distinct, deterministic identity while leaving the world dirty flag and selection restoration intact.

## Controller-observed RED (before implementation)

Rendering editor, filter `Cortex.Frontend.InputArea.Resolution`: 9 tests, 4 failed.

1. `ProviderEditorContextNotDuplicatedByAutoContext` — `## Editor Context (auto)` 1 vs 2; identity 1 vs 2.
2. `CodexDefaultTurnStatesAccessModeOnce` — `Current access mode:` 1 vs 2.
3. `DisabledAutoContextStillResolvesProviderChipOnce` — identity 1 vs 3 (auto-context off).
4. `NextTurnUsesCurrentSelection` — fixture defect above plus the downstream assertion.

## Controller-observed GREEN (after implementation)

PENDING — controller-owned. The rendering editor must be built and the brief's direct invocation run
over the same filter; there are no expected failures after this change (the 4 RED tests flip, the 5
other matching tests stay GREEN).

## Task 17 handoff (not done here)

* The parent-repo `docs/systems/cortex-frontend.md` is a parent-local document and stays out of the
  plugin commit; Task 17 owns consolidated docs/toolkit sync (consistent with the other tasks).
* No command surface or tool signature changed, so MCP fallback/signature fixtures are unaffected.
