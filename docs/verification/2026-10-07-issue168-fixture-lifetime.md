# Issue 168 — fixture lifetime verification

Issue: https://github.com/etelyatn/UnrealCortex/issues/168 (reported by @etelyatn).

Owning repository: UnrealCortex. Base: `32cd5f07f5df91d9555508a8c8118393384512bd`; origin/main was refreshed before publication and remained this revision. Windows x64, installed UE5.8.3, supported MSVC14.44. Design and implementation: [design](../plans/2026-10-07-issue168-fixture-lifetime-design.md), [implementation](../plans/2026-10-07-issue168-fixture-lifetime-impl.md).

## Root evidence and limits

The retained original minidump identifies null resolved `UClass::ClassGeneratedBy` references in the mutable Killable GC batch, not a demonstrated dangling pin. Four null queue entries have mutable slots at class+`0x130`; their following CDO slots are class+`0x170`. The CDO's captured ClassPrivate independently ties each pair to the class. Engine PDB layout identifies those two offsets as ClassGeneratedBy and ClassDefaultObject. Fault instruction reads InternalIndex at null+`0xc`.

| Queue entry | Class-owner mutable slot | Captured following CDO | Captured CDO ClassPrivate |
|---|---|---|---|
|5|0x1a2ad27c530|0x1a46f7d0500|0x1a2ad27c400|
|7|0x1a2ad27d330|0x1a46f7d0a00|0x1a2ad27d200|
|14|0x1a2ad52cc30|0x1a2a2508200|0x1a2ad52cb00|
|16|0x1a2ad52da30|0x1a46f7da000|0x1a2ad52d900|

Original evidence remains in workspace `Saved/Crashes/UECC-Windows-4A06EE924124DBB9580BDBBBD14A1638_0000/`: original automation log, CrashContext.runtime-xml and UEMinidump.dmp. Controller inspected the retained dump with bundled LLDB and independently reparsed captured queue/CDO memory after resuming. Exact originating fixture names and the nulling writer/interleaving were not captured. Validation-to-conversion nulling is a source-backed explanation, not a proven engine-wide race repair. Historical matching signatures are not claimed to share one root.

The consumer-relevant deterministic regression proves the existing fixture helper left generated/skeleton classes and both CDOs live after retiring only their Blueprint/package. The correction retires the owned object graph while keeping non-class reflection metadata available to ordinary destruction. The regression compiles an event-bearing fixture, retires it, then compiles an unrelated Blueprint with default GC. Weak checks ignore garbage flags, so successful post-compile checks establish actual destruction rather than merely repeating the retirement flags. It preserves the unrelated Blueprint and avoids accessing raw retired objects after GC.

Compression warnings are an independent fixture lifetime problem: async DDC validation reads the live model. Asset-scoped completion after initial model population and between undo/redo transitions prevents overlap with the next model state. No causal relationship to the original GC crash is claimed.

## RED and correction history

Workspace evidence copies are retained in `Saved/Issue168/` to survive the test runner's normal log rotation. They are local evidence, not files shipped by this plugin PR.

- `AutomationTest_2026-10-07_215547.log`: deterministic RED, retirement regression0/1; four class/CDO retirement assertions fail with the original helper.
- `...215844.log`: rendering RED1741/1742, same single retirement failure; no reproduced compression warnings.
- `...220643.log`: intermediate CurveAuthoring+5/5, but two native compression warnings during first AddSetRemoveReadback mutation. Undo/redo-only barriers were insufficient; initial fixture compilation also needs completion.
- `...221247.log`: intermediate blanket descendant garbage marking33/34 in RemoveGraph+, with persistent-frame teardown ensure. Non-class UField metadata must not be forcibly nulled before CDO destruction. The final helper releases those fields to normal GC; this ensure is not suppressed.
- The original current-target rendering baseline passed1741/1741 with two compression warnings. Its raw `212744.log` was later rotated by the runner; this observation was read at the time and is not misrepresented as a still-retained raw file. Isolated warning guards can pass because the async warning timing is nondeterministic; no deterministic warning RED is claimed.

## Final observed verification

All native runs were controller-owned, serial, through the project runner. Both changed modules rebuilt successfully with no compiler warnings. The initial baseline build had eight existing Core/Data C4996 deprecation warnings; this test-only correction does not claim to remove them.

| Final run | Passed | Total | Native warnings | Unexpected native errors | Preserved log suffix |
|---|---:|---:|---:|---:|---|
|Retirement regression, actual post-compile destruction|1|1|0|0|222015.log|
|CurveAuthoring+|5|5|0|0|221133.log|
|RemoveGraph+|34|34|0|0|222252.log|
|Complete rendering Cortex+|1742|1742|0|0|222427.log|

Complete rendering run completed in213.91 seconds. Actual log command line: `-ExecCmds="Automation RunTests Cortex+" -unattended -nopause -nosplash -noloadstartuppackages -nosound -log ...`; no `-NullRHI`. The real Editor run passed original `RecoveryFaults`, the new retirement consumer, AddSetRemoveReadback and UndoRedo. No fatal or ensure matches.

The full raw log contains **one deliberate native Error severity**: ExclusivePort's second TCP bind must fail on ports18900–18999. `Source/CortexCore/Private/Tests/CortexTcpServerTest.cpp:419–433` declares exactly one AddExpectedError; that test succeeds. The report therefore claims zero **unexpected** native errors, not zero raw Error strings. Startup text such as `LogTemp: Error test:` is not `Error:` severity. No warning/error suppression was added.

## Frozen reviewed/tested source identity

Source remained unchanged through final rendering acceptance and the independent source review. Main-branch base above plus these source SHA256 values bind the test result to the candidate; publication records the resulting commit SHA.

| Source file | SHA256 |
|---|---|
|CortexBPRemoveGraphPersistenceTest.cpp|a46137230253befee105bfe9daa84e23590bbcdd29846c32d2c6e3ca951d7909|
|CortexAnimationCurveAuthoringTest.cpp|279bafb5306c8773fc42284b454ab35b69be4e5a70d13ee55161916bec7175ca|

Built DLL SHA256: CortexBlueprint `589d5541d6020a89da292c0d4bfa3852ca2c60071dc243afde3fca6ea712a287`; CortexAnimation `61e453ba15305a742396becee110db62d472109077c396afa96b4983372cea8e`.

Two independent read-only reviews reported no consequential findings: native implementation/lifetime/test validity, and diagnostic attribution/approved scope. Neither reviewer ran tests; controller acceptance above is independent of their source assessments.

## Teardown and integration boundaries

The test runner stopped its own Editor. After final acceptance, PID40544 remained running with the CortexSandboxMirror project command line. No unrelated Editor or service was terminated. No engine changes, GC disabling, sleeps, retries, DDC purge, global transaction reset, global object cleanup, production/API/schema/capability/MCP/toolkit changes, or parent gitlink commit. System architecture and command catalog documentation are intentionally unchanged.

## Simplicity check

Existing fixture owners and native suites remain the integration points. One scoped cleanup traversal and asset-specific compilation boundaries replace unsafe partial retirement/async overlap; no new shared framework, module dependency, public API or compatibility path.
