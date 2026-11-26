# Issue 245: Cortex-started PIE bypasses the editor's background CPU throttle — implementation

**Design:** `2026-11-26-issue245-pie-cpu-throttle-bypass-design.md`
**Request:** <https://github.com/etelyatn/UnrealCortex/issues/245>

## Task 1: lease (CortexEditor)

- `Private/CortexEditorCPUThrottleBypass.h/.cpp`: `FCortexEditorCPUThrottleBypassLease`, non-copyable.
  The constructor increments a module-static count and, on the first lease with `GEditor` present, adds
  `UEditorEngine::FShouldDisableCPUThrottling::CreateStatic` (returns true) to
  `GEditor->ShouldDisableCPUThrottlingDelegates`, storing its `GetHandle()`. The destructor decrements
  and, at zero, removes the entry by handle.

## Task 2: PIE tracker owner

- `FCortexEditorPIEState` gains `TUniquePtr<FCortexEditorCPUThrottleBypassLease> CPUThrottleBypass`
  and `HoldCPUThrottleBypass()` (idempotent). `SetState(Stopped)` resets the lease.
- `FCortexEditorPIEOps::StartPIE` calls `HoldCPUThrottleBypass()` immediately before
  `SetState(Starting)` and `RequestPlaySession`.

## Task 3: owned physical-input PIE owner

- `FCortexEditorPhysicalInputSession` (public header forward-declares the lease) gains
  `TUniquePtr<FCortexEditorCPUThrottleBypassLease> CPUThrottleBypass`.
- `BeginOwnedPIE` acquires it before `RequestPlaySession`.
- Released where ownership ends: `TickInternal` when the owned teardown latches resolved, `Shutdown()`
  when nothing owned remains, `BindTarget` (borrowed binding), and the destructor.

## Tests

`Private/Tests/CortexEditorCPUThrottleTestUtils.h` counts vetoing delegates exactly as
`ShouldThrottleCPUUsage()` evaluates them (that function itself returns false in automation).

| Test | Proves |
|---|---|
| `Cortex.Editor.PIE.CPUThrottleBypass.CortexStartedSessionOnly` | Engine-started session: no veto. Cortex `StartPIE`: veto from admission and while playing; released by `stop_pie`; a second Cortex session ended by the engine (`RequestEndPlayMap`, as a toolbar stop) releases it too |
| `Cortex.Editor.PhysicalInputOwnedCPUThrottleBypass` | Owned PIE: veto from `BeginOwnedPIE` admission and while ready; released after the owned teardown resolves |

RED before Tasks 1-3: both tests failed on every "vetoes" assertion (0 instead of 1).
