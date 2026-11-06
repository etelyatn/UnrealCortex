# Issue 168 — Blueprint fixture retirement and curve transition barriers

## Requirement

[Issue 168](https://github.com/etelyatn/UnrealCortex/issues/168), reported by @etelyatn, records a rendering `Cortex+` GC crash during the initial `BP_Recovery` compile. Identify the retained-object/GC root, retain a failing-before/passing-after regression, and verify the complete rendering suite with safe teardown and no unexpected native warnings/errors. Do not disable GC, suppress warnings, add sleeps, or clean unrelated Editor state.

The user approved merged delivery and the bounded fixture-lifetime correction described here. This is test-infrastructure work, not a change to Blueprint/Animation authoring contracts.

## Observed root and limits

The original minidump proves a null reference inside the **killable mutable** GC batch, not a dangling pin:

- Fault: `movl 0xc(%rax), %ecx`, with `rax=0`, in `TReferenceBatcher<FMutableReference,FResolvedMutableReference,...>::FlushQueues`.
- The validated queue contains 25 references. Entries 5, 7, 14, and 16 have `Object=null` and mutable slots at `0x1a2ad27c530`, `0x1a2ad27d330`, `0x1a2ad52cc30`, and `0x1a2ad52da30`.
- Engine PDB layout places `UClass::ClassGeneratedBy` at `+0x130` and `ClassDefaultObject` at `+0x170`. The next entry in each pair points to a live CDO whose `ClassPrivate` equals the null slot minus `0x130`. These are therefore `ClassGeneratedBy` slots of retained Blueprint classes.
- The existing `MarkFixtureGarbage` helper marks only the Blueprint and package as garbage. The regression proves that both generated/skeleton classes and both CDOs remain live after this partial retirement.

Engine source validates mutable reference values before converting them into resolved snapshots. Both reflected `UClass` properties and its deferred reference collector can report class-owner references. A validation-to-conversion nulling window is a source-backed explanation; the dump does **not** record the exact nulling writer/interleaving. Historical identical crash signatures precede issue 167 but do not establish one cause for every historical crash.

The correction removes the demonstrated partially retired fixture object graph. It does not claim to repair every possible engine GC batching race or every other module's teardown.

## Decision

1. Keep the existing private `MarkFixtureGarbage` owner. Its callers create dedicated packages; enumerate only descendants of that package and clear `RF_Public | RF_Standalone`. Mark Blueprint, generated/skeleton classes, CDOs, graphs, nodes, owned templates, and the package as garbage. Reflection fields other than classes are released to normal GC rather than forced to garbage: CDO destruction still needs their metadata. Do not scan or retire unrelated packages/classes, reset transactions globally, or initiate global GC during teardown.
2. Add one regression around the helper. Establish an event-bearing compiled fixture, generated/skeleton classes and CDOs; assert retirement while an unrelated Blueprint remains intact, then compile that unrelated Blueprint using default compilation/GC. Check weak references while ignoring garbage flags to distinguish actual destruction from merely marking objects. Isolate the failing-before run with test-owned cleanup before exposing the partial graph to another compile.
3. Treat the animation compression warnings separately. An async DDC result is validated against the live sequence model. Finish compilation for **only the curve fixture sequence** after initial model population and between undo/redo transitions, while retaining the existing curve/key readback assertions. Count warnings with a thread-safe output-device counter; never alter log severity or expected-warning declarations.

The current-target rendering run emitted two warnings during UndoRedo. Subsequent instrumented isolated/full RED runs did not reproduce that timing. After adding undo/redo barriers, the affected curve suite instead exposed the same warning pair during the first AddSetRemoveReadback mutation, before any undo. This establishes that initial model population also needs its asset-scoped completion boundary. The deterministic primary RED/GREEN regression is the four-object Blueprint retirement failure; warning assertions guard both observed curve consumers without claiming deterministic reproduction of every async interleaving.

The first blanket descendant-retirement implementation failed the broader RemoveGraph suite: forcing `UFunction` metadata to garbage caused `UBlueprintGeneratedClass::DestroyPersistentUberGraphFrame` to see mismatched frame-property/function pointers during CDO destruction. The correction keeps those fields available for normal destruction instead of forced pointer nulling. The independent suite caught this defect; it is not suppressed or classified as an expected ensure.

## Boundaries

- Production operations, MCP commands, capabilities, schemas, transactions, and engine source remain unchanged.
- No new shared abstraction, module dependency, compatibility layer, or toolkit update.
- No parent repository gitlink commit and no unrelated Editor shutdown.
- Default GC remains enabled in the consumer compile; final acceptance uses rendering `Cortex+` without `-NullRHI`.
- Deliberately expected native error-path tests are classified separately from unexpected diagnostics; do not claim the raw full-suite log contains no `Error:` strings.

## Acceptance

- Four retirement assertions fail before and pass after the helper correction.
- Normal subsequent Blueprint compilation succeeds and collects retired classes/CDOs while preserving the unrelated fixture.
- Affected RemoveGraph and curve-authoring tests pass without warnings.
- Complete rendering `Cortex+` finishes without unexpected warnings/errors/fatals/ensures.
- Independent source/evidence review approves the exact candidate before publication and expected-head-protected integration.
