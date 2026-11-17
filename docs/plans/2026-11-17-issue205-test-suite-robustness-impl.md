# Issue 205 implementation: test-suite robustness

Design: `2026-11-17-issue205-test-suite-robustness-design.md`. Test code only.

## Changes

1. Unique fixture suffixes: `.Left(N)` -> `.Right(N)` on `FGuid::NewGuid().ToString(EGuidFormats::Digits)` in
   - `CortexData/.../CortexDataTypeReplacementTestSupport.h` (`FFixture::Suffix`, item 3),
   - `CortexUMG/.../CortexUMGRenameDependentsTest.cpp` (8 sites, item 4),
   - `CortexReflect/.../CortexReflectIssue122TestSupport.h`, `CortexBlueprint/.../CortexBPDeleteTest.cpp`, `CortexBPParentRulesTest.cpp`, `CortexCore/.../CortexAssetDeletionTest.cpp` (same latent collision).
2. `CortexBPCompileTest.cpp` (`Cortex.Blueprint.Compile.Basic`):
   - `FPackageName::DoesPackageExist("/Game/Blueprints/BP_SimpleActor")` gates the pre-built block (`AddInfo` skip), and `Result.Data` is dereferenced only after `TestTrue(Data.IsValid())`.
   - Records whether `/Game/Maps/TestMap` was loaded before the Level Script cases; if the test loaded it, `ClearWorldComponents()` + `CleanupWorld()` on the inactive initialized world at the end.
3. `CortexAnimationMontageAuthoringTest.cpp`: `SectionSelector(double Index, ...)`.
4. Platform assumptions:
   - `Cortex.Reflect.Config.UnreadableFiles`, `Cortex.Reflect.NameLookup.UnreadableFiles`, `Cortex.Blueprint.MigrationExport.PairConsistency`: `if constexpr (!PLATFORM_WINDOWS)` `AddInfo` + return.
   - `Cortex.Blueprint.Redirectors.Fixup`, `Cortex.Blueprint.Rename.Basic`, `Cortex.Blueprint.Rename.BatchSwap`: expected-error occurrences `PLATFORM_WINDOWS ? 1 : -1`.
   - `CortexReplayTestUtils`: `bool CortexReplaySkipWithoutPhysicalKeyState(FAutomationTestBase&)` (`#if PLATFORM_WINDOWS` false; otherwise `AddInfo` + true), called first in 57 tests: `Lifecycle` 48, `Window` 6, `RoundTrip` 2, `Overlay` 1.
5. `CortexBPAddVariableTest.cpp` (`Cortex.Blueprint.AddVariable.InheritedPropertyCollision`): GUID-named directory/asset as in `AddVariable.Basic`, early return when `create` fails, `delete` (`force: true`) at the end.

## How the Replay set was chosen

On Windows, `CanResolvePhysicalKeyState()` in `CortexEditorPhysicalInputSession.cpp` was temporarily forced to return false (the non-Windows branch) and `Cortex.Replay` was run: exactly 57 of 177 tests failed, each on a refused capture/replay (`Owned capture admitted`, `Run reached Error`, `INVALID_OPERATION` instead of the expected code). Those 57 call the helper. With the helper also forced to the non-Windows branch the suite ran clean; both instruments were then reverted. The reporter's Linux run counted 52 (`Lifecycle` 43, `Window` 7, `RoundTrip` 2); the emulation is a superset for `Lifecycle` and covers one `Window` test fewer, which could not be identified without a Linux run.

## Verification

See `docs/verification/2026-11-17-issue205.md`.
