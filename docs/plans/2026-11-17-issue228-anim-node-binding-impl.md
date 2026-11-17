# Issue 228 implementation: anim node Binding on creation

Design: [2026-11-17-issue228-anim-node-binding-design.md](2026-11-17-issue228-anim-node-binding-design.md).

## Changes

- `Source/CortexGraph/Private/Operations/CortexGraphNodeOps.{h,cpp}`: new `FCortexGraphNodeOps::PlaceNewAnimNode(Node)`. It calls `PostPlacedNewNode()` only when `FCortexGraphBindingStorage::IsAnimNode(Node)` is true and the node has a `UAnimBlueprint` outer. `graph.add_node` calls it after construction params and before `AllocateDefaultPins`.
- `Source/CortexGraph/Private/Operations/CortexGraphPatchOps.cpp`:
  - The planning owner is a transient `UAnimBlueprint` when the target is an Anim Blueprint, and a `UBlueprint` otherwise.
  - The planned node and the applied node both call `PlaceNewAnimNode` before pin allocation.

## Tests

`Source/CortexGraph/Private/Tests/CortexGraphAnimNodeBindingTest.cpp` drives the public router.

- `Cortex.Graph.AnimNodeBinding.AddNode`, on a template Anim Blueprint:
  - ModifyBone and TwoWayBlend get the default-class Binding, outered to the node.
  - A float variable is wired to TwoWayBlend's `Alpha`, and the blend is wired to the root. The Blueprint compiles cleanly.
  - The compiled node's exposed-value handler is the property-access handler.
- `Cortex.Graph.AnimNodeBinding.ApplyPatch`:
  - Preview then apply of ModifyBone and BlendListByInt succeeds, with readback matched.
  - Both nodes get the Binding.
  - BlendListByInt's pose pin count equals that of a node placed the `FGraphNodeCreator` way.
- `Cortex.Graph.AnimNodeBinding.NonAnimBlueprint`: `graph.add_node` of TwoWayBlend into an Actor Blueprint's EventGraph still succeeds and does not assert.

## Behavior changes visible to callers

Anim graph nodes created by `graph.add_node` and `graph.apply_patch` in an Anim Blueprint now behave like editor-placed nodes:

- Linked pins drive the node at runtime.
- The Anim Blueprint extensions the node needs are registered.
- Subclass placement applies, for example BlendListByInt and MultiWayBlend get their editor pose count.

Nodes created before this fix keep no Binding until they are recreated, or until the workaround in the issue is applied.

## Verification

See [../verification/2026-11-17-issue228.md](../verification/2026-11-17-issue228.md).
