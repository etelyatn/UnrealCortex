# Issue 233 implementation plan: `anim.sample_runtime_pose` and `get_animbp_info include:["nodes"]`

**Goal:** sample PIE bone poses over game time, and report per-node input sources and fast path.

**Design:** `2026-11-26-issue233-anim-runtime-pose-design.md` (contract, acceptance, rejected alternatives).

**Base:** UnrealCortex `e0d98efa` (0.4.24), branch `feat/233-anim-runtime-pose`. Executed inline (native), TDD per task; one reviewer over the branch.

## Global constraints

- Module boundaries: CortexAnimation adds a private dependency on CortexEditor (shared infrastructure) only.
- All UObject access on the game thread; no `LogTemp`; zero warnings in `Cortex.Animation+`, `Cortex.QA+`.
- Native tests for the sampler run under NullRHI (transient game world, no PIE).

## Task 1: shared deferred PIE poll

- Move `StartDeferredPIEPoll` and `FPIEPoll` from `Source/CortexQA/Private/CortexQAUtils.{h,cpp}` to `Source/CortexEditor/Public/CortexPIEUtils.h` / `Private/CortexPIEUtils.cpp` (`FCortexPIEUtils::StartDeferredPIEPoll`, `FCortexPIEUtils::FPIEPoll`), unchanged behavior; migrate the four calls in `CortexQAActionOps.cpp`.
- Proof: build; `Cortex.QA.PIE.DeferredOpsLifecycle` still passes (rendering suite).

## Task 2: pose sampler core (`Private/Operations/CortexAnimRuntimeOps.{h,cpp}`)

Interfaces:

- `enum class ECortexAnimPoseSpace : uint8 { Local, Component, DeltaFromRef };`
- `struct FCortexAnimPoseRequest { TWeakObjectPtr<AActor> Actor; TWeakObjectPtr<USkeletalMeshComponent> Mesh; TArray<FName> Bones; TArray<FName> Properties; ECortexAnimPoseSpace Space; int32 SampleCount; double IntervalSeconds; double TimeoutSeconds; };`
- `class FCortexAnimPoseSampler { FCortexAnimPoseSampler(FCortexAnimPoseRequest, double StartRealSeconds); TOptional<FCortexCommandResult> Step(UWorld& World, double RealSeconds); };`
- `FCortexAnimRuntimeOps::ParseRequest(UWorld* World, const TSharedPtr<FJsonObject>& Params, FCortexAnimPoseRequest& Out, FCortexCommandResult& OutError) -> bool`
- `FCortexAnimRuntimeOps::SampleRuntimePose(const TSharedPtr<FJsonObject>& Params, FDeferredResponseCallback) -> FCortexCommandResult`

Tests (`Private/Tests/CortexAnimationRuntimePoseTest.cpp`, fixture: transient game world, `ASkeletalMeshActor` with `SKM_Manny_Simple`, `VisibilityBasedAnimTickOption = AlwaysTickPoseAndRefreshBones`; new native `UCortexAnimTestRuntimeInstance` with `float Elapsed` accumulated in `NativeUpdateAnimation` and `bool bBlendActive`):

- `Cortex.Animation.RuntimePose.RefPose`: native instance (no graph). `delta_from_ref` angles 0 and translations 0 for `pelvis`, `hand_r`; `local` equals `GetRefPoseTransform`; `properties.Elapsed` strictly increases.
- `Cortex.Animation.RuntimePose.Playing`: `PlayAnimation(MF_Unarmed_Jog_Fwd, true)`; each `component` sample equals `GetSocketTransform(bone, RTS_Component)` read right after that step; first and last `hand_r` samples differ.
- `Cortex.Animation.RuntimePose.GameTime`: interval 0.02, dilation 1 vs 0.5 at 0.01 s ticks; spacing ≥ interval; dilated run takes ~2x steps.
- `Cortex.Animation.RuntimePose.Lifecycle`: actor destroyed mid-run → `completed:false`, `stop_reason:"target_destroyed"`, samples kept; RealSeconds past timeout → `stop_reason:"timeout"`.
- `Cortex.Animation.RuntimePose.Refusals`: `ParseRequest` codes from the design table; handler without PIE → `PIE_NOT_ACTIVE`.

## Task 3: handler, registration, MCP

- Route `sample_runtime_pose` in `CortexAnimationCommandHandler.cpp` with its `FCortexCommandInfo` params; update `CortexAnimationRegistrationTest`.
- `MCP/src/cortex_mcp/tools/routers.py` `_dispatch_timeout`: `anim.sample_runtime_pose` → `timeout_seconds` (default 60, non-finite/bool → default) + 30. `capabilities.py`: add to `_ANIM_INSPECTION_COMMANDS`, hint line. Tests in `tests/test_routers.py`, `tests/test_capabilities.py`.

## Task 4: `include:["nodes"]`

- `FCortexAnimBindingReport::AppendNodes(UAnimBlueprint*, int32 Limit, const TSharedPtr<FJsonObject>& Data, FCortexCommandResult&) -> bool`; accept `nodes` in `CortexAnimInspectOps.cpp` include parsing.
- Tests (`CortexAnimationNodesReportTest.cpp`, template AnimBP fixtures from `CortexAnimationTestAnimBlueprint.h`): `PlayRate` bound to `NativeSpeed` → `bound`, `does_not_use_blueprint`, `fast_path:true` after compile; `PlayRate` linked to a `GetNativeSpeed` call → `linked` with `linked_to`, `not_compiled` before compile and `uses_blueprint`/`fast_path:false` after; exposed unlinked `PlayRate` → `unconnected` with `default_value`; `nodes` absent without `include`.

## Task 5: docs, verification, release

- `docs/systems/cortex-animation.md` (CortexSandbox), verification report `docs/verification/2026-11-26-issue233.md`, live MCP smoke (PIE + `set_time_dilation 0.05`, `ABP_Unarmed` nodes), toolkit sync, patch bump to 0.4.25.
