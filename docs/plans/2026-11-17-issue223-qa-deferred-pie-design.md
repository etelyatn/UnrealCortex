# Issue 223: QA observe_state limits, deferred transport timeouts, PIE end during a wait

Request: https://github.com/etelyatn/UnrealCortex/issues/223, reported by @XpycT. The report reviewed the code at `f72a0ad` (0.4.6).

## Outcome and boundary

`qa.observe_state` refuses limits that make no sense and bounds the size of its response. `qa.interact`, `qa.move_to` and `qa.wait_for` report their own documented result even when the request runs past 30 s. If PIE ends while one of them is pending, it answers `PIE_TERMINATED` right away. The Python tools and the command declarations do not change.

## Source basis

At base `bd2328fc` (0.4.12):

1. `CortexQAWorldOps.cpp` cast `max_actors` to `int32` and called `NearbyActors.SetNum(MaxActors)` whenever `Num() > MaxActors`. With `-1` that condition is always true. UE 5.8 then kills the editor with a fatal error, `Trying to resize TArray to an invalid size of 4294967295` (`ContainerHelpers.cpp:8`, `OnInvalidArrayNum`); this was observed live on a 0.4.5 editor. A NaN value made the cast undefined. The response had no upper bound. `radius` and `interaction_range` were not checked either.
2. `interact`, `move_to` and `wait_for` returned `bIsDeferred` without `DeferredTimeoutSeconds`, so `FCortexTcpServer` applied `DefaultDeferredTimeoutSeconds` (30 s). The Python tools already wait `timeout + 5 s` (`tools/qa/actions.py`), which means the server cut the request off first. `interact` clamps `duration` to 30 s, so a 30 s hold always lost that race.
3. All three polled on the PIE world's `FTimerManager`. Ending PIE destroys the world and its timers. The callback was then dropped without being called, the `PIE_TERMINATED` branches never ran, and the client waited for the generic timeout. RED observed this for `interact` and `move_to`.

## Selected design

1. **Shared poll helper.** `FCortexQAUtils::StartDeferredPIEPoll(World, Interval, OperationSeconds, Label, Callback, Poll)` registers an `FTSTicker` core ticker, which keeps ticking after PIE teardown. On each tick it does the following:
   - If the weakly held world is gone or is no longer `GEditor->PlayWorld`, it answers `PIE_TERMINATED` "PIE terminated during {Label}" and removes itself.
   - If the world is paused, it skips the tick. The old world timers did not run while paused either.
   - Otherwise it runs `Poll`. The first result it returns answers the command.

   The helper returns the deferred result with `DeferredTimeoutSeconds = max(OperationSeconds, 0) + 5 s`. The 5 s grace matches the Python tools.
2. **Ops.** `interact` passes `duration`, and `move_to` and `wait_for` pass `timeout`. Their completion payloads and error messages are unchanged. The core ticker fires at most once per frame in real time and does not catch up, while the old looping world timer caught up in dilated game time. `move_to`'s direct (no navigation) movement therefore advances by the PIE game time elapsed since the last poll, at the same 60 units per game second as before. Without that, its speed would depend on frame rate.
3. **observe_state.** `radius`, `max_actors` and `interaction_range` must be finite and `>= 0`. Otherwise the command returns `INVALID_VALUE`. This check runs before the PIE check, so a bad request gets the same answer in every editor state. A `max_actors` above 200 is clamped to 200 rather than rejected (maintainer decision), so existing callers that pass large values keep working.

Rejected: completing pending callbacks from `FEditorDelegates::PrePIEEnded`. That needs a registry of pending operations. The ticker does the same job with no shared state.

## Acceptance

- `observe_state` with negative `max_actors`, `radius` or `interaction_range` returns `INVALID_VALUE`.
- `max_actors` 1000 with more than 200 observable actors returns exactly 200.
- `interact` (duration 30), `move_to` (timeout 45) and `wait_for` (timeout 60) each return a deferred timeout longer than the requested span.
- Ending PIE while all three are pending answers each one with `PIE_TERMINATED` within 5 s.
