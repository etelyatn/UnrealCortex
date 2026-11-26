# Issue 245: Cortex-started PIE bypasses the editor's background CPU throttle (design)

**Date:** 2026-11-26
**Status:** Implemented
**Request:** <https://github.com/etelyatn/UnrealCortex/issues/245> (found during #227)

## Problem

`a8062fc8` added `_inject_fps_throttle` (`t.MaxFPS 0`, `t.UnfocusedFrameRateLimit 0` after
`editor.start_pie`) only to Python files the server never registered; #227 deleted them. The issue asks
whether AI-driven PIE sessions should disable FPS throttling and, if so, to implement it on the live path.

Porting the injection would not work on UE 5.8:

- `t.MaxFPS` already defaults to `0` (UnrealEngine.cpp `CVarMaxFPS`).
- `t.UnfocusedFrameRateLimit` does not exist in the engine source.
- `t.IdleWhenNotForeground` (the 2026-02 request note) only applies when `FApp::IsGame()`
  (`FEngineLoop::ShouldUseIdleMode`), not to the editor.

The actual cap is the editor's background CPU throttle. `UEditorEngine::GetMaxTickRate` returns 3 FPS
whenever `ShouldThrottleCPUUsage()` is true (EditorEngine.cpp ~2526-2563). That is true when the
editor is neither the foreground application nor focused and `UEditorPerformanceSettings::
bThrottleCPUWhenNotForeground` ("Use Less CPU when in Background") is set, which is the default
(EditorPerformanceSettings.cpp:21). An AI client drives PIE from another window, so its session runs at
3 FPS: input sequences, `wait_for`, movement and timing checks crawl. Automation runs never see it:
`ShouldThrottleCPUUsage()` returns false under `-unattended` and `GIsAutomationTesting`.

## Decision

Yes, for **Cortex-started** PIE sessions only (maintainer decision on the issue). A human's own PIE
session keeps the engine default.

The engine provides the opt-out: `UEditorEngine::ShouldDisableCPUThrottlingDelegates`. While any bound
delegate returns true, `ShouldThrottleCPUUsage()` returns false.

- `FCortexEditorCPUThrottleBypassLease` (CortexEditor, private) is an RAII lease. The first live lease
  adds one delegate returning true; the last one removes it by handle. Game thread only.
- `FCortexEditorPIEState` holds a lease from `editor.start_pie` admission (also the start phase of
  `restart_pie`) until the tracker enters `Stopped`. Every end path reaches `Stopped`: `stop_pie`, an
  engine/toolbar end (`EndPIE`), a cancelled or rejected request (deferred `CancelPIE` cleanup), and
  the stale-world reset in `StartPIE`. Engine-initiated sessions (`PreBeginPIE` without a Cortex start)
  never take a lease.
- `FCortexEditorPhysicalInputSession` holds a lease from `BeginOwnedPIE` (owned PIE used by Cortex
  Replay) until its owned teardown resolves, until `Shutdown()` finds nothing owned, until `BindTarget`
  drops ownership, or until it is destroyed.

### Rejected alternatives

| Option | Why not |
|---|---|
| Port the console-command injection to `start_pie_session` | No effect on UE 5.8 (see above) |
| Clear `bThrottleCPUWhenNotForeground` | Persistent user setting; changes non-Cortex sessions and the idle editor |
| Veto while any PIE runs | Changes PIE behavior for humans who switch away; the issue scopes this to AI-driven sessions |

## Contract

- No command, parameter, response or schema change. `editor.start_pie`/`restart_pie` and Cortex
  Replay keep their contracts; the effect is the editor's tick rate while the editor is in the
  background.
- The veto never outlives the Cortex-owned session. A borrowed physical-input binding never holds it.
