# PR #136 Portable Build Implementation Plan

> **For agentic workers:** Use executing-plans inline; independently review before integration. Steps use checkboxes for tracking.

**Goal:** Integrate the contributor's Linux-build corrections without dropping current-main callsites or regressing Windows behavior.

**Architecture:** Reuse CortexEngineCompat::JsonKeyToString for FString consumers; deduce the actual FJsonObject map entry type for all Values iteration. Gate only LiveCoding dependency/include using the engine's existing target/macro policy. No command/schema/persistence changes.

**Tech Stack:** UE5.8.3 C++, UBT, MSVC14.44, installed Clang18.1.8 with MSVC14.38 headers, PowerShell, existing Unreal automation and native TCP.

**Spec:** https://github.com/etelyatn/UnrealCortex/pull/136 and 2026-10-03-pr136-portable-build-design.md.

## Global Constraints

- Normal submodule branches; no worktrees, reset, clean, stash or force-push.
- Preserve contributor attribution and unrelated workspace work.
- All shell commands through rtk; engine D:/UnrealEngine/UE_5.8.
- No alternate compatibility layer or change to supported command behavior.
- Linux not locally available; distinguish direct Windows/Clang condition evidence from contributor Linux evidence.

## Review Focus

- Shared-string keys: no temporary TPair binding or hidden FString-container comparison failures.
- Unknown JSON field diagnostics retain the original key and error contract.
- Nested graph literals/readback and inventory forwarding retain values and names.
- WITH_LIVE_CODING false compiles without LiveCoding headers/dependency; true retains existing behavior.
- Explicit FString TMaps/TArray buffers remain typed rather than mechanically rewritten.

## Task 1: JSON key compilation compatibility

Files: submitted JSON loop fixes in Blueprint Asset/ClassDefaults/Component ops; Core CredentialStore/AssetOps; Data ImportQueue/JsonDiff; Graph CommandHandler/PatchOps; Level Actor/Component/Organization; StateTree State/Transition ops. Current main additionally has three incompatible loops in GraphCommandHandler RewireInventory and GraphMigrationOps CountJsonNumbers/EncodedResponseChars.

Consumes: existing FJsonObject::Values and CortexEngineCompat::JsonKeyToString. Produces: same commands, values, diagnostics and inventories; valid deduced map-entry binding.

- [x] Run real Clang baseline compilation before changes; inspect original and new callsite diagnostics. Compiler integration is the failing regression harness, not a source-text test.
- [x] Fetch exact submitted head3daecfe7018e0a1de4200c9e6cff5124d52c2249; integrate against current main77238c195664ca040fd02b1fc46b1378208ac1f2 on a normal branch.
- [x] Actual current-main diagnostics confirmed the three new loops. Changed only their `const TPair<FString, TSharedPtr<FJsonValue>>&` to `const auto&`, preserving bodies. No new helper or runtime test that merely pins syntax.
- [x] Repeat actual Clang source compilation; run all existing Core/Data/Blueprint/Graph/Level/StateTree/frontend regression suites in one sequential rendered Cortex+ invocation (1673/1673,0failed,2 unrelated Animation CurveAuthoring.UndoRedo warnings); exercise JSON-sensitive native commands in task-owned editors in both MSVC configurations. No full Clang-link success claim.

## Task 2: LiveCoding platform gate

Files: Source/CortexFrontend/CortexFrontend.Build.cs and Private/Widgets/SCortexConversionTab.cpp.
Consumes: Target.bWithLiveCoding and WITH_LIVE_CODING. Produces: conditional existing engine dependency/include only.

- [x] Inspect existing guarded callsites and engine target defaults.
- [x] Retain submitted gate implementation; successful full MSVC non-unity builds with NoLiveCoding (592 actions,414.04s) and normal LiveCoding-enabled configuration (592 actions,430.62s). No unrelated build fixes or diagnostic suppression.
- [x] Run frontend regression tests disabled (333/333) and in enabled full Cortex+ (1673/1673 overall); observe actual CortexFrontend startup in both native editors. Model/argument tests do not establish LiveCoding toggle restoration or real conversion file writes.

## Integration acceptance

- [x] Independent JSON and LiveCoding source/evidence review approved, including maintainer compiler red/green, actual retained logs, warnings/skips and condition limits. Neither reviewer ran validation.
- [x] Exact heads/base/candidates and exercised evidence/limits recorded in docs/verification/2026-10-03-pr136.md; existing systems/toolkit intentionally unchanged because APIs and architecture are unchanged.
- [x] Additively patched the authorized contributor branch from3daecfe to521d650 without force-push. Original PR #136 merged with an exact-head guard; contributor history preserved, no replacement required.
- [x] Confirmed remote PR #136 MERGED at c5a8d4de6c2ceaec4bec5a132a842cd06644066c and fast-forwarded plugin main; parent gitlink and unrelated work preserved. Evidence-only publication uses the same compiled source.

## Rulings / execution ledger

Ruling: Use inline adoption and compiler red/green, not ritual reimplementation of submitted code. The three additional loops are necessary current-main consumers of the same compatibility correction; only edit after real compiler evidence. No permanent syntax-pinning test. No Linux-success claim without a Linux engine/SDK.

Controller compiler red: Windows UE5.8.3, Clang18.1.8 with MSVC14.38 headers, `-DisableUnity -NoLiveCoding`, 592 actions, failed OtherCompilationError after1162.85s. Preserved log: project Saved/Logs/PR136ClangBaseline.log. Current-main failures at GraphCommandHandler:182 and GraphMigrationOps:9028/9056 explicitly diagnose `-Werror,-Wrange-loop-construct`; submitted sites fail the same way.

Independent JSON reviewer inspected the exact three-line maintainer delta and the remaining explicitly FString-owned buffers/maps: equivalent correction, no findings. LiveCoding reviewer inspected contributed gates against engine target policy: no findings. Runtime/green acceptance remains controller-owned.

Disabled-condition evidence must follow the consumed header: Definitions.CortexFrontend.h includes SharedDefinitions.UnrealEd.Project.ValApi.ValExpApi.Cpp20.h, located under project Intermediate/Build/Win64/x64/CortexSandboxEditor/Development/UnrealEd; its WITH_LIVE_CODING is0. The unused older frontend Definitions.h is not valid condition evidence.

Post-patch Clang result: all actual source compilation completed without C++ error diagnostics, including both Graph files and SCortexConversionTab, across the577-action build. Full build still failed at linking (`Saved/Logs/PR136ClangCompileAndLink.log`,1176.69s). The installed MSVC-built engine cannot resolve Clang-specific Windows namespace imports: engine Core/Public/Microsoft/MinimalWindowsApi.h:15-19 uses CORE_API under Clang instead of extern-C imports, and Core/Private/Microsoft/MinimalWindowsApi.cpp:25-188 defines those namespace wrappers only for a Clang-built engine. Source-compilation acceptance is not full-build success. No engine shim, diagnostic suppression or engine rebuild is in scope; verify full disabled/enabled builds with supported MSVC.

Integration merge candidate:8c235036daf8fe7c349ca3ed2c80f41e4b06a74f. Verified three-line maintainer correction:521d65081ef3940ee1fb11338ad24664a0e3c0ab. Disabled-side verification used identical source contents before that recording commit; enabled-side verification uses that source commit.

Disabled MSVC build evidence: project Saved/Logs/PR136MSVCNoLiveCodingBuild.log and PR136NoLiveCodingDefinitions.h (`WITH_LIVE_CODING=0`). Native editor PID47416,port8742: successful CortexFrontend startup and tab/menu registration in PR136DisabledSmoke.log. Cortex.Frontend+ passed333/333 with no automation warning/error records (AutomationTest_2026-10-03_112514.log); MarkdownWidget size assertions skipped because no layout pass occurred.

Enabled MSVC build evidence: project Saved/Logs/PR136MSVCLiveCodingBuild.log and PR136LiveCodingDefinitions.h (`WITH_LIVE_CODING=1`). Native editor PID47140,port8742: successful CortexFrontend startup and tab/menu registration in PR136EnabledSmoke.log.

Both native configurations passed nested Unicode-key JSON comparison (one changed row), ignored-field comparison (zero changed rows), unknown Unicode top-level field rejection with exact INVALID_FIELD details.field, and owned Blueprint bReplicates class-default mutation false-to-true with native readback. Both task-owned editor services stopped; fixture files/assets and throwaway smoke script removed. Existing compiler deprecation warnings remain.

Enabled full-suite log: project Saved/TestLogs/AutomationTest_2026-10-03_113506.log, rendered UnrealEditor.exe without NullRHI,1673/1673 passed,0failed,2warnings from the unchanged Animation.CurveAuthoring.UndoRedo compressed-curve fixture (lines3838-3854). No warning suppression or unrelated animation fix was imported. Relevant JSON consumers, rewire public-route inventory forwarding/complete bridge-budget inventory and frontend ModuleLoad passed. Content/platform/layout/locked-map assertion skips are explicitly recorded in the verification report; total passed is the runner's reported count, not a claim every assertion ran.

Final independent reviews approved source and evidence package with no findings. Reviewers independently inspected retained MSVC summaries, consumed macro snapshots, startup events, focused JSON test events and warning/skip attribution; native smoke execution remains controller-owned. Source publication is approved; documentation publication follows exact remote source outcome.

Remote source outcome: original #136 MERGED, head521d65081ef3940ee1fb11338ad24664a0e3c0ab, merge c5a8d4de6c2ceaec4bec5a132a842cd06644066c. GitHub reported CLEAN/MERGEABLE with no checks before routine merge; no protection bypass, force push, contributor-branch deletion or parent commit. Maintainer fork write was authorized by maintainerCanModify and succeeded additively. Documentation is published independently after this exact outcome; no source changes after tested candidate.
