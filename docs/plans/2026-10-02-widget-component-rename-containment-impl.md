# Widget component rename containment implementation

## Changes

- `Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp`: extend the existing external-reference preflight with native generated member names from the existing UI-component extension. Inline storage covers the bare widget and ordinary small component sets; component lookup is skipped when there are no dependents.
- `Source/CortexUMG/Private/Tests/CortexUMGWidgetVariableTest.cpp`: add `Cortex.UMG.RenameWidgetComponentDependent`, using a real navigation component, compiled generated property, and external Blueprint getter. Assert compound-only reference semantics and engine discovery before exercising refusal and preservation. Clean dependent package/cache ownership on every exit.
- `UnrealCortex.uplugin`: patch release `0.3.2` to `0.3.3`, integer version `14` to `15`.
- `Source/CortexBlueprint/Private/Tests/CortexBPRemoveGraphPersistenceTest.cpp`: shared cleanup now clears the Blueprint's public/standalone flags and marks the Blueprint itself garbage before marking its package. Extend synthetic metadata recovery coverage to assert that discarded assets are no longer valid editor compilation candidates.
- `Source/CortexData/Private/Tests/CortexBatchCommandTest.cpp`: delete obsolete zero-command-success subcase; canonical `Cortex.Core.Batch.EmptyBatch` owns the current refusal contract.
- `Source/CortexFrontend/Private/Tests/SCortexInputAreaTest.cpp`: delete the ambient-context/Markdown-wording `EmptyProviderSilentDrop` test rather than re-pinning implementation wording or changing production context behavior.

## TDD evidence

- RED: `Saved/TestLogs/AutomationTest_2026-10-02_202140.log`, one test failed with four expected assertions: rename incorrectly succeeded, containment code absent, widget renamed, component target changed. Real property lookup and dependent discovery succeeded.
- GREEN: native build succeeded; `Saved/TestLogs/AutomationTest_2026-10-02_202445.log`, all three `Cortex.UMG.RenameWidget` tests passed, including component containment, original rename coverage, and slot-first animation binding undo/redo.
- Read-only review of the correction found no remaining blocker in the UMG slice. Guarded cleanup/retirement review also found no concrete blocker.
- Fixture lifetime RED: `Saved/TestLogs/AutomationTest_2026-10-02_205304.log`, one expected failure because package-only cleanup left the recovery Blueprint valid.
- Fixture lifetime GREEN: native build succeeded; `Saved/TestLogs/AutomationTest_2026-10-02_205523.log`, the recovery and cleanup-lifetime test passed without warnings/errors.
- Instrumented broad-run evidence: `Saved/PR156NativeDebug.log`; CodeLLDB stopped in `UBlueprintGeneratedClass::InitializeFieldNotifies` under PIE's `ResolveDirtyBlueprints`. The synthetic fixture's FieldNotify state remained live after package-only cleanup. This does not establish the cause of the earlier, separate GC access violation.

Full candidate acceptance is recorded separately in `docs/verification/2026-10-02-pr-156.md`; focused regression results alone do not establish full merge acceptance.
