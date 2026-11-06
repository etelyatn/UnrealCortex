# Issue 168 — fixture lifetime implementation

Design: [fixture lifetime correction](2026-10-07-issue168-fixture-lifetime-design.md).

## Blueprint retirement

`Source/CortexBlueprint/Private/Tests/CortexBPRemoveGraphPersistenceTest.cpp`:

- Add `UObject/UObjectHash.h` and keep the existing private cleanup helper and all its callers.
- `MarkFixtureGarbage` visits descendants of the fixture's dedicated package with `ForEachObjectWithOuter`. It clears public/standalone flags and marks owned objects and the package as garbage, except non-class `UField` reflection metadata, which remains available to CDO destruction and is released through normal GC. It performs no explicit collection, unrelated-object traversal, transaction reset, rename, or file cleanup.
- `Cortex.Blueprint.RemoveGraph.Fixture.RetiresOwnedObjectGraph` compiles an event-bearing fixture, establishes generated/skeleton classes and CDOs, observes retirement, and preserves an unrelated Blueprint and its class owner. Its positive path compiles the unrelated Blueprint with default GC and checks weak references with `bThreadsafeTest=true`, which ignores garbage flags and distinguishes actual object destruction from retirement.
- The regression avoids raw retired-object access after the consumer compile's GC. Its failing-before cleanup retires only test-owned descendants before returning, so the RED run does not contaminate subsequent tests.

## Animation fixture boundaries

`Source/CortexAnimation/Private/Tests/CortexAnimationCurveAuthoringTest.cpp`:

- Complete only the new fixture sequence's initial compilation immediately after model population. This prevents an empty-model cache result from being validated after the first curve is authored.
- In UndoRedo, use `UE::Anim::IAnimSequenceCompilingManager::FinishCompilation` for the one fixture sequence after each mutation, undo, and redo. There are no sleeps, retries, DDC purges, global compilation drains, or GC-mode changes.
- Reuse the warning output device for AddSetRemoveReadback and UndoRedo; use `FThreadSafeCounter` because async compression can log from worker threads. Existing curve/key readback and transaction assertions remain.
- Warnings are observed rather than suppressed. The initial rendering baseline and the later AddSetRemoveReadback fixture run emitted the same compression warning pair; isolated instrumented runs can pass because the race is timing-dependent.

## Verification commands

Build the supported `CortexSandboxEditor Win64 Development` target with `-NoLiveCoding -NoHotReloadFromIDE`; keep unrelated Editors running.

Run each native filter serially through the project's PowerShell runner:

```powershell
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.Blueprint.RemoveGraph.Fixture+' -Timeout 600 }"
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.Animation.CurveAuthoring+' -Timeout 600 }"
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.Blueprint.RemoveGraph+' -Timeout 600 }"
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex+' -Timeout 600 }"
```

The full filter must select rendering and omit `-NullRHI`. Distinguish expected deliberate error-path logging from unexpected diagnostics. The retirement regression is the deterministic RED/GREEN and subsequent-consumer smoke; a green full run alone is not a root-cause regression.

## Unchanged owners

No production implementation, engine source, command/schema/capability, MCP code, toolkit, architecture documentation, or parent gitlink update. The original class-slot dump identifies the failing GC reference; the precise worker nulling interleaving remains an inference, not a claimed engine-wide repair.
