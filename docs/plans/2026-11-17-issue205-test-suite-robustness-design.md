# Issue 205 design: test-suite robustness on hosts other than CortexSandbox/Windows

Issue: https://github.com/etelyatn/UnrealCortex/issues/205 (reported by @Boshchuk from a Linux UE 5.8.2 run).

All seven items are test defects; no plugin production code changes.

## Causes and decisions

| Item | Cause | Decision |
|---|---|---|
| 1. `Compile.Basic` crashes without `BP_SimpleActor` | `Cortex.Blueprint.ContentSetup` sorts after `Compile`; the test dereferenced `Result.Data` unchecked (`SharedPointer.h:1133`). | Follow the existing content-dependent convention (`CortexBPAnalysisTest`): when `/Game/Blueprints/BP_SimpleActor` does not exist, `AddInfo` and skip that block; otherwise only dereference `Data` once it is valid. Renaming tests to force order was rejected: UE has no ordering contract and Core/Reflect dependents run in other namespaces. |
| 2. `TestMap` world left initialized | When `/Game/Maps/TestMap` is not the editor world, the Level Script cases load it; `UEditorEngine::OnAssetLoaded` initializes the inactive world. A later GC destroys `LandscapeSubsystem` before `UWorld::BeginDestroy` runs its own cleanup, so `WorldSubsystem.cpp:118` ensures. | If the test loaded the map, it ends with `ClearWorldComponents()` + `CleanupWorld()` on the inactive initialized world, the same sequence `UEditorEngine::PreExit` uses. |
| 3, 4. `TypeReplacement.VerifyBlocked` poisoning and the UMG rename crash | Fixture suffixes came from `FGuid::NewGuid().ToString(Digits).Left(8)`. On Linux `NewGuid` is UUIDv7 (`UnixPlatformGuid.cpp`): `Guid.A` is the millisecond timestamp >> 16, constant for ~65 s, so consecutive tests got the same `/Game/Temp/Issue122_<A>` and `CortexRenameDependents/<A>_Owner` paths. | Same fix as `6b069fc8`: take the suffix with `.Right(N)` (random bits on every platform). Applied to the 13 remaining `.Left(N)` sites, including four in Blueprint/Core/Reflect not yet reported. |
| 5. Montage oversized selector UB | `SectionSelector(int32 Index, ...)` received `INT32_MAX + 1.0`. | The helper takes `double`, so the oversized value reaches the command's `RawIndex > INT32_MAX` check. |
| 6a. `Reflect.Config.UnreadableFiles`, `Reflect.NameLookup.UnreadableFiles`, `Blueprint.MigrationExport.PairConsistency` | They rely on Windows share modes (an open writer/reader blocks reading/replacing). | `if constexpr (!PLATFORM_WINDOWS)` `AddInfo` skip. |
| 6b. `Redirectors.Fixup`, `Rename.Basic`, `Rename.BatchSwap` | Expected "package was marked as deleted in editor" exactly once; Linux never logs it. | Occurrences `PLATFORM_WINDOWS ? 1 : -1`: Windows keeps the exact count, other platforms ignore it. |
| 6c. Replay tests that need physical key state | `CanResolvePhysicalKeyState()` is false off Windows by design, so capture/live replay are refused. | A test helper `CortexReplaySkipWithoutPhysicalKeyState` (`AddInfo` + skip off Windows) at the top of each test that drives live capture or replay. The set was measured, not guessed: see the implementation guide. |
| 7. `AddVariable.InheritedPropertyCollision` fixed path | Fixed `/Game/Temp/CortexBPTest_AddVarComponent/...` and no file deletion: every direct run left the asset, so the next run's `create` failed. `RunTests.ps1` hid it by deleting `Content/Temp/Cortex*Test*` first. | Same pattern as `AddVariable.Basic`: GUID-named directory and asset, `delete` with `force` at the end. |

## Non-goals

- No change to `blueprint.compile` for Level Script Blueprints of non-open maps. The inactive-world ensure is reachable from production as well (any `__level_bp__:` compile of a map that is not the editor world, followed by GC), but it is an engine destruction-order issue and outside this issue's test scope; recorded in the verification report.
- No Linux build or run was available; Linux-only causes are verified from engine source and the reporter's observations, and Replay is verified by emulating the non-Windows capability on Windows.
