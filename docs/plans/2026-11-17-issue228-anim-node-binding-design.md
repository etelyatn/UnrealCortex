# Issue 228: Cortex-created anim graph nodes must get the editor's Binding sub-object

Request: https://github.com/etelyatn/UnrealCortex/issues/228, reported by @etelyatn (UnrealCortex 0.4.5, UE 5.8).

## Outcome and boundary

An anim graph node created by Cortex must be initialised like one placed in the editor. Graph logic wired into its pins must then reach the node at runtime. No new command, parameter or response field. The optional readback flag suggested in the issue (report anim nodes with no Binding in `describe_node`/`get_subgraph`) is out of scope by maintainer decision: once creation is fixed, a missing Binding can only come from foreign or legacy assets.

## Source basis

At base `dc30b811` (0.4.10):

- `graph.add_node` (`CortexGraphNodeOps.cpp`) and `graph.apply_patch` create nodes with `NewObject`, apply construction params and allocate pins. The patch route does this twice: once for a transient planned node in a planning graph whose owner is a transient `UBlueprint`, and again for the real node in the apply phase. Both routes call `PostPlacedNewNode` only for `UK2Node_Composite`.
- In the UE 5.8 editor, `FGraphNodeCreator::Finalize` (`EdGraph.h`) calls `PostPlacedNewNode` and then `AllocateDefaultPins`. `UBlueprintNodeSpawner` allocates pins first and then places the node. Either way the node is placed.
- `UAnimGraphNode_Base::PostPlacedNewNode` (`AnimGraphNode_Base.cpp:217`) does two things. It runs `EnsureBindingsArePresent`, which creates `Binding` from `UAnimBlueprint::GetDefaultBindingClass()` or falls back to `UAnimGraphNodeBinding_Base`, and only when the node has an Anim Blueprint outer. It also runs `UAnimBlueprintExtension::RequestExtensionsForNode`, which calls `GetAnimBlueprint()` (a `CastChecked<UAnimBlueprint>`) and therefore asserts when the owner is not an Anim Blueprint.
- Subclasses extend placement. For example, `AnimGraphNode_BlendListByInt` adds a second pose and reconstructs, and `AnimGraphNode_MultiWayBlend` adds two poses.
- The anim compiler only processes a node's non-pose pins through `Binding->ProcessDuringCompilation` → `UAnimBlueprintExtension_Base::ProcessNonPosePins`. With no Binding, a linked pin compiles to no property-access copy record. The node keeps its stored default, and compilation reports nothing.

## Selected design

`FCortexGraphNodeOps::PlaceNewAnimNode(Node)` calls the virtual `PostPlacedNewNode()` for an anim graph node whose outer chain contains a `UAnimBlueprint`. `FCortexGraphBindingStorage::IsAnimNode` detects anim nodes by resolving `UAnimGraphNode_Base` by path, so CortexGraph still links no animation module. The helper runs after construction params and before pin allocation, the `FGraphNodeCreator` order. It is called in three places:

- `graph.add_node`. An anim node class sent to an ordinary Blueprint keeps its previous result and never reaches the asserting extension request.
- The patch planning phase. The planning owner is created with the target's class (`UAnimBlueprint` for an Anim Blueprint target, `UBlueprint` otherwise). Subclass placement therefore adds the same pins to the planned node that apply will create, and the extension request lands on the transient owner, not the real asset. Without this, the preview omits placement pins and readback rolls the apply back.
- The patch apply phase.

Rejected: calling `PostPlacedNewNode` for every node class. That would change existing K2 node behaviour across all graphs, which this issue does not ask for. Also rejected: refusing anim node classes outside Anim Blueprints in `graph.add_node`. It would change an existing contract, and the patch route already refuses them through its graph compatibility check.

## Acceptance

- `graph.add_node` of `AnimGraphNode_ModifyBone` (the reported class) and of `AnimGraphNode_TwoWayBlend` into an AnimGraph produces a node whose `Binding` is the Anim Blueprint's default binding class and is outered to the node.
- A float member variable linked to the added node's `Alpha` pin compiles cleanly into a property-access exposed-value handler on the compiled node. This is the copy that runs before the node evaluates.
- `graph.apply_patch` (preview then apply) of `AnimGraphNode_ModifyBone` and `AnimGraphNode_BlendListByInt` succeeds with readback matched. Both nodes get the Binding, and BlendListByInt has the pose pins of an editor-placed node.
- `graph.add_node` of an anim node class into an ordinary Actor Blueprint returns its previous result and does not assert.
- Live editor: a node added through the MCP route has a Binding. It survives save and reload.
