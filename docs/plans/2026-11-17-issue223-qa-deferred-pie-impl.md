# Issue 223 implementation: QA deferred PIE polling and observe_state limits

Design: [2026-11-17-issue223-qa-deferred-pie-design.md](2026-11-17-issue223-qa-deferred-pie-design.md).

## Changes

| File | Change |
|---|---|
| `Source/CortexQA/Private/CortexQAUtils.h/.cpp` | Adds `FCortexQAUtils::StartDeferredPIEPoll`: an `FTSTicker` poll that holds the world weakly, skips paused frames, answers `PIE_TERMINATED` once PIE ends or is replaced, and returns a deferred result whose transport timeout is the operation span plus 5 s. |
| `Source/CortexQA/Private/Operations/CortexQAActionOps.cpp` | `interact`, `move_to`, `wait_for` (including the `delay` form) now poll through the helper. The PIE world timers and `GEditor` checks are gone. The success and timeout payloads are unchanged. |
| `Source/CortexQA/Private/Operations/CortexQAWorldOps.cpp` | `observe_state` validates `radius`, `max_actors` and `interaction_range` (finite and `>= 0`, otherwise `INVALID_VALUE`) before the PIE check, and clamps `max_actors` to 200. |
| `Source/CortexQA/Private/Tests/CortexQAWorldTest.cpp` | `Cortex.QA.World.ObserveState.InvalidLimits` (no PIE). |
| `Source/CortexQA/Private/Tests/CortexQADeferredPIETest.cpp` | `Cortex.QA.PIE.DeferredOpsLifecycle`: the deferred timeouts exceed the requested spans, and all three ops answer `PIE_TERMINATED` after PIE ends. `Cortex.QA.PIE.ObserveStateMaxActorsClamp`: 210 spawned actors with `max_actors` 1000 return 200. |

## Verification

Run `RunTests.ps1 'Cortex.QA+'` with rendering (the script drops `-NullRHI` for QA). Evidence is in [../verification/2026-11-17-issue223.md](../verification/2026-11-17-issue223.md).
