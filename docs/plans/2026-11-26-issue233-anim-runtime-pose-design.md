# Issue 233: sample runtime bone poses over game time, and anim-node fast-path report

Request: https://github.com/etelyatn/UnrealCortex/issues/233, reported by @etelyatn (UnrealCortex 0.4.5, UE 5.8).

## Outcome and boundary

1. `anim.sample_runtime_pose`: an agent verifying procedural animation can read the pose of chosen bones of a running PIE skeletal mesh repeatedly over **game** time (so `set_time_dilation` slows the sampling with the game), in local, component or delta-from-reference space, with chosen anim-instance properties logged at every sample. This replaces hand-written `editor.run_python` loops over `get_socket_transform`/`get_ref_pose_transform`; a mid-blend swing that the final pose hides becomes visible in the table.
2. The "related small ask": for every anim node, the pins that feed it at runtime (linked, bound, or unconnected) and whether the node is on the fast path. Node settings themselves already ship as `graph.get_node_settings` (issue 231, 0.4.24); this slice adds only the missing input-source and fast-path view, and names the node so `graph.get_node_settings` can read its settings.

Maintainer decisions (2026-11-26): full delivery (PR, merge, issue closure, toolkit sync); the small ask is included.

Out of scope: playback control, writing poses, sampling non-PIE worlds, per-binding fast-path (the engine decides fast path per node, see below).

## Source basis (UE 5.8, base `e0d98efa`, 0.4.24)

- No command reads a runtime pose. `statetree.get_runtime_state` (CortexStateTree) is the existing read-only PIE reader; `qa.wait_for` and friends answer deferred through `FCortexQAUtils::StartDeferredPIEPoll` (CortexQA private): a core ticker that reports `PIE_TERMINATED` when the PIE world goes away, skips paused frames, and sets `DeferredTimeoutSeconds = operation + 5 s`. CortexAnimation may depend on the shared infrastructure module CortexEditor, not on CortexQA.
- Pose readers: `USkinnedMeshComponent::GetSocketTransform(Bone, RTS_ParentBoneSpace | RTS_Component)` (what the reporter called from Python), `GetRefPoseTransform(BoneIndex)` (local reference pose), `GetBoneIndex`. A mesh with `VisibilityBasedAnimTickOption = AlwaysTickPose` does not refresh bones while unrendered, so a sample of an off-screen mesh can be stale: `WasRecentlyRendered()` is reported per sample.
- `UWorld::Tick` scales delta by `AWorldSettings::GetEffectiveTimeDilation()`; `UWorld::GetTimeSeconds()` is dilated game time.
- Fast path: the compiler stores the result per editor node in the transient `UAnimGraphNode_Base::BlueprintUsage` (`EBlueprintUsage::NoProperties | DoesNotUseBlueprint | UsesBlueprint`, AnimGraphNode_Base.h:113-118, 215-216), set in `UAnimBlueprintExtension_Base::HandleCopyTermDefaultsToDefaultObject` (AnimBlueprintExtension_Base.cpp:55-107) from whether the node's exposed-value handler has a `BoundFunction` (a Blueprint thunk). This is what the editor's fast-path lightning icon and the "uses Blueprint to update its values" warning read. It is transient: a node that has linked or bound inputs but still reports `NoProperties` was not compiled in this editor session.
- `UAnimGraphNode_Base::HasBinding(PinName)` (public) is the engine's own pin-binding predicate; `UAnimationGraphSchema::IsPosePin` identifies pose links.

## Contract: `anim.sample_runtime_pose` (read-only, deferred)

| Param | Rule |
|---|---|
| `actor` | Label, name or path (`FCortexPIEUtils::FindActorInPIE`). Exactly one of `actor`/`actor_class`. |
| `actor_class` | Class name (`BP_Hero_C`, `BP_Hero` also matches `_C`) or class path; matches actors whose class or any super class matches. 0 matches `ACTOR_NOT_FOUND`; >1 `AMBIGUOUS_ACTOR` with `details.candidates` (≤20). |
| `mesh_component` | Optional component name. Omitted: the actor's only `USkeletalMeshComponent` with a mesh; none `COMPONENT_NOT_FOUND`, several `AMBIGUOUS_COMPONENT` with candidates. |
| `bones` | Required, 1-64 unique names present on the mesh; unknown bone `INVALID_FIELD` (`details.bone`). |
| `space` | `local` (default, parent-bone space) \| `component` \| `delta_from_ref`. |
| `sample_count` | 1-300, default 10. |
| `interval_seconds` | Game seconds between samples, 0-60, default 0.1; 0 samples every frame. |
| `properties` | Optional, ≤32 anim-instance property names; unknown `PROPERTY_NOT_FOUND`; requested without an anim instance `INVALID_OPERATION`. |
| `timeout_seconds` | Real-time budget 1-1800, default 60. |

No PIE: `PIE_NOT_ACTIVE`. All validation happens before deferral. The first sample is taken on the first frame after the call; each later sample waits until at least `interval_seconds` of game time have passed since the previous one (one sample per frame at most, so a frame hitch never bunches samples; actual `game_time` is reported). Paused frames take no sample, but the poll keeps running while paused so the real-time `timeout_seconds` still answers with the samples taken (review finding, 2026-11-26).

Response: `world`, `actor`, `actor_class`, `mesh_component`, `skeletal_mesh`, `anim_instance_class` (or null), `space`, `interval_seconds`, `sample_count` (requested), `completed`, `stop_reason` (`completed | timeout | target_destroyed`; a stop keeps the samples taken), `samples[]`: `index`, `game_time`, `time_dilation`, `recently_rendered`, `bones` (object keyed by bone: `translation {x,y,z}`, `rotation {pitch,yaw,roll}`, `scale {x,y,z}`; `delta_from_ref` adds `angle_degrees`), `properties` (object keyed by name, values as `FJsonObjectConverter`).

`delta_from_ref`: with local `L` and reference `R` (both parent-bone space), `translation = L.T - R.T`, `scale = L.S - R.S`, rotation `Q = L.Q * R.Q⁻¹` (shortest arc, so `angle_degrees` ∈ [0,180]).

PIE ending mid-run: `PIE_TERMINATED` (the shared poll contract). The MCP router gives the command a receive window of `timeout_seconds` (default 60) + 30 s.

## Contract: `get_animbp_info include:["nodes"]`

One entry per anim node (graph then node order, `binding_limit` bounded, standard `count/returned/truncated/items`): `node_id` (the name `graph.get_node_settings` takes), `node_guid`, `node_class`, `graph_guid`, `graph_name`, `node_struct`, `blueprint_usage` (`no_properties | does_not_use_blueprint | uses_blueprint | not_compiled | unreachable`), `fast_path` (true for `does_not_use_blueprint`, false for `uses_blueprint`, null otherwise), `inputs[]` per non-pose input data pin: `pin`, `hidden`, `source` (`bound` when `HasBinding(pin)`, else `linked` with `linked_to[] {node_id, node_guid, node_class, pin_name}` (the `property_access` link shape plus `node_id`), else `unconnected` with `default_value`).

The engine value `NoProperties` on a node with a linked or bound input has two causes: the Blueprint changed since its last compile (`Status` not up to date) → `not_compiled`, compile for the real value; or the compiler pruned the node because it feeds no output pose (`FAnimBlueprintCompilerContext` resets every node, then `PruneIsolatedAnimationNodes`) → `unreachable`, the node never runs and recompiling does not change it.

## Selected design

- Move `StartDeferredPIEPoll` from `FCortexQAUtils` (private) to `FCortexPIEUtils` (CortexEditor public); QA callers migrate; no alias left behind.
- `CortexAnimation` gains a private dependency on `CortexEditor`. New `Private/Operations/CortexAnimRuntimeOps.{h,cpp}`: `FCortexAnimRuntimeOps::ParseRequest(UWorld*, Params, ...)` (validation, synchronous) and `FCortexAnimPoseSampler`, whose `Step(UWorld&, double RealSeconds)` returns the final result when done; the handler wires it to the shared poll. Tests drive `Step` on a transient game world with explicit ticks and time dilation (NullRHI safe, like the StateTree runtime test).
- Nodes report: `FCortexAnimBindingReport::AppendNodes`, reusing its node collection and locators.

Rejected: a second private ticker in CortexAnimation (duplicates the poll lifecycle); real-time sampling (time dilation would not slow it); a full settings dump in `get_animbp_info` (duplicates `graph.get_node_settings`); per-binding fast-path flags (the engine stores none after compile).

## Acceptance

1. A skeletal mesh playing an animation: samples over time differ for a moving bone; `local`/`component` equal `GetSocketTransform` in that space; a mesh without animation reports `delta_from_ref` zero (angle 0) and `local` equal to the reference pose.
2. Game-time spacing: consecutive `game_time` differ by ≥ `interval_seconds`; with time dilation 0.5 the same sampling needs twice the frames.
3. `properties` values track the anim instance over samples.
4. Refusals: no PIE, both/neither actor selectors, unknown/ambiguous actor, missing/ambiguous component, unknown bone, bad space/ranges, unknown property.
5. Lifecycle: target destroyed mid-run returns `completed:false, stop_reason:"target_destroyed"` with the samples taken; real-time timeout returns `stop_reason:"timeout"`; PIE end returns `PIE_TERMINATED` (QA lifecycle test keeps covering the moved poll).
6. `include:["nodes"]` on a compiled fixture: a bound input is `bound`, a linked input is `linked` with its source, an exposed unlinked pin is `unconnected`; a node fed through a Blueprint thunk is `uses_blueprint`/`fast_path:false`, a binding-only node `does_not_use_blueprint`/`true`.
7. Live editor over MCP: PIE with time dilation 0.05 samples a Mannequin bone mid-animation; `include:["nodes"]` on `ABP_Unarmed`.
