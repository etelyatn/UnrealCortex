# Issue 255: enforce declared command parameters — implementation

**Design:** `2026-11-19-issue255-declared-params-enforcement-design.md`
**Request:** <https://github.com/etelyatn/UnrealCortex/issues/255>

## Global constraints

- Unreal 5.6+ (verified on 5.8). Code style follows Epic C++ conventions. Logging goes to `LogCortex`.
- The refusal is `INVALID_FIELD` and its message is `Unknown param '<key>'. Accepted params: <a, b | none>`.
- Keys with a leading `_` are always accepted.

## Task 1: router-owned enforcement (CortexCore)

**Files:**

- `Public/CortexCommandRouter.h`: `FCortexRegisteredDomain` gains
  `TMap<FString, FCortexCommandInfo> Commands`, filled once in `RegisterDomain`. A private
  `const FCortexCommandInfo* FindCommandInfo(const FString& Namespace, const FString& Command) const`
  is added.
- `Private/CortexCommandRouter.cpp`, `Execute`: after the domain match and the null-params
  substitution, if `Domain.Commands.Find(SubCommand)` succeeds, call
  `FCortexCommandContract::ValidateParams` and return its refusal. `IsRollbackSafeCommand` uses the
  same cached map.
- `Public/CortexCommandContract.h`, `Private/CortexCommandContract.cpp`: `ValidateParams` always
  checks unknown keys and checks required presence only when `Info.bStrictParams` is set. An empty
  declaration is described as `none`. `ValidateDeclaredParams` is removed.
- `Public/ICortexDomainHandler.h`: the `StrictParams()` and `bStrictParams` comments say what the
  marker now means.
- Remove the per-domain `ValidateDeclaredParams` calls in the Blueprint, Data, Reflect and UMG
  handlers.

**Test:** `Private/Tests/CortexRouterDeclaredParamsTest.cpp`, tests
`Cortex.Core.Router.DeclaredParams.{UnknownParamRefusedBeforeHandler, CommandWithoutParamsRefusesAnyKey,
RequiredPresenceOnlyForStrictCommands, BatchStepRefusedIndividually}`.

- RED: build and run `Cortex.Core.Router.DeclaredParams+`. The unknown-key, bare-command and batch
  tests are expected to fail.
- GREEN: the same run passes.

## Task 2: declaration audit per domain

There is one owner per handler file, and each owner fixes its own Python callers and native tests:

| Slice | Handler files |
|---|---|
| Blueprint and Graph | `CortexBPCommandHandler.cpp`, `CortexGraphCommandHandler.cpp` |
| UMG and Reflect | `CortexUMGCommandHandler.cpp`, `CortexReflectCommandHandler.cpp` |
| Level, QA and Replay | `CortexLevelCommandHandler.cpp`, `CortexQACommandHandler.cpp`, `CortexReplayCommandHandler.cpp` |
| Material and Gen | `CortexMaterialCommandHandler.cpp`, `CortexGenCommandHandler.cpp` |
| Data and Core | `CortexDataCommandHandler.cpp`, `CortexCoreCommandHandler.cpp` (declaration table only) |
| Editor, Animation and StateTree | `CortexEditorCommandHandler.cpp`, `CortexAnimationCommandHandler.cpp`, `CortexStateTreeCommandHandler.cpp` |

Each command is resolved by one of the rules in the design: add a declaration for every key the
handler reads, remove every declared key it never reads, and report any open-ended keys. The
controller integrates the slices and makes the shared-file changes itself.

## Task 3: capability mirror

With the editor running on the new build, `get_capabilities` refreshes
`Saved/Cortex/capabilities-cache.json`. Copy it to `MCP/tests/fixtures/capabilities_cache_full.json`,
then run `uv run python scripts/sync_fallback.py --from-fixture`.

## Task 4: verification and delivery

1. Build `CortexSandboxEditor`. The native suites must pass with zero warnings:
   `Cortex.Core+`, `Cortex.Data+`, `Cortex.Graph+`, `Cortex.Blueprint+`, `Cortex.UMG+`, `Cortex.Reflect+`,
   `Cortex.Animation+`, `Cortex.StateTree+` and `Cortex.Gen+` run under NullRHI. `Cortex.Level+`,
   `Cortex.Editor+`, `Cortex.QA+` and `Cortex.Replay+` need rendering.
2. Python: the `not e2e and not scenario and not stress` set, then `e2e` and `scenario` against the live
   editor.
3. Live MCP benchmark (`cortex-mcp-benchmark`). Then the issue #234 item 1 probe:
   `editor.get_recent_logs` with two bogus params must return `INVALID_FIELD`.
4. Clang syntax gate on the changed C++.
5. Sync the toolkit with `cortex-sync-toolkit`. Toolkit docs or skills that teach an undeclared param
   are corrected.
6. Bump the patch version, open a PR, review, merge, and close issue 255. Then mark TD-MCP-005
   resolved in the CortexSandbox tech-debt file.
