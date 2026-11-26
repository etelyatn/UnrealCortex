# Issue 231: structured read/write of anim node settings

Request: https://github.com/etelyatn/UnrealCortex/issues/231, reported by @etelyatn (UnrealCortex 0.4.5, UE 5.8).

## Outcome and boundary

Callers can read and write the settings of an animation graph node (the `Node` struct of a `UAnimGraphNode_*`, e.g. `BoneToModify`, `RotationMode`, `AlphaInputType`, `AlphaBoolBlend`, arrays of structs on custom nodes) without `editor.run_python` text round-trips. A write validates bone names against the Anim Blueprint's target skeleton, can be previewed, can be guarded by a fingerprint, and leaves the node with the pins an editor edit would leave (no stale pins until reload).

Maintainer decisions (2026-11-26): reconstruction is automatic inside the write; no standalone `graph.reconstruct_node`. Delivery is end to end.

Out of scope: nested Anim Blueprint graphs (state, transition, conduit graphs). They stay read-only for every graph mutator (`ResolveMutableNodeGraph`); the new commands follow the same rule. Compile and save stay explicit, as for `graph.set_pin_value`.

## Source basis

At base `3467df96` (0.4.22):

- `graph.set_pin_value` reaches only pins. `anim_cmd` covers sequences, montages, notifies, curves and sockets, not graph nodes. No command reads or writes a node's own reflected properties.
- `UAnimGraphNode_Base` keeps its runtime node in one `FStructProperty` whose struct derives from `FAnimNode_Base` (engine `GetFNodeProperty` finds it the same way). `FAnimNode_Base`, `FBoneReference` and `UAnimBlueprint::TargetSkeleton` live in `Engine`, so CortexGraph still links no AnimGraph module; the node class is matched by path (`FCortexGraphBindingStorage::IsAnimNode`).
- Editor edits go through `PostEditChangeProperty`, and node classes react there. `UAnimGraphNode_SkeletalControlBase::PostEditChangeProperty` (AnimGraphNode_SkeletalControlBase.cpp:414) handles `AlphaInputType`: it breaks links on the alpha pins going away and calls `ReconstructNode`, whose `CustomizePinData` shows `bAlphaBoolEnabled` and hides `Alpha`. A raw struct write skips all of this; that is the reported stale-pin defect.
- A member's value comes from elsewhere at compile when it exists as a pin, hidden pins included (`AnimBlueprintCompiler.cpp:470-490`, array elements as `Name_N` at :430-445), or has an active property binding, which the compiler registers independently of pins (`AnimBlueprintExtension_Base.cpp:398-420`). A shown optional-pin record of an empty exposed array creates element pins on reconstruction. A struct write to any of these is silently lost.
- `FCortexSerializer::JsonToProperty`/`PropertyToJson` (CortexCore) own JSON <-> property conversion. `JsonToStruct` only warns on unknown fields, does not check edit flags of nested fields and resizes arrays to the supplied length; converting `null` on an instanced sub-object retires the object, which a scratch copy shares with the live node (`CortexSerializer.cpp:79-87,1869-1878`).
- `graph.add_node` creates nodes without `RF_Transactional` (`CortexGraphNodeOps.cpp`), so `Modify()` records nothing; the alpha reaction also edits the node's `Binding` map without `Modify()`.
- The asset fingerprint `graph.set_pin_value` uses does not change between two unsaved edits of an already-dirty asset (`DirtyEpoch` is not advanced).

## Contract

`graph.get_node_settings` (read, rollback-safe, strict params): `asset_path`, `node_id`, optional `graph_name` (default EventGraph, like other node reads), `subgraph_path`, `property_path`. Returns `node_id`, `node_class`, `node_struct`, `settings` (the editable members as an object, or the value at `property_path`), `pin_properties` (editable members whose value comes from a pin or binding: write them with `graph.set_pin_value`), `target_skeleton` (path or empty) and `fingerprint` (the asset fingerprint plus `node_settings_hash`: the MD5 of the UTF-8 JSON of `settings`, whose numbers keep full precision, followed by the struct's exported text, which covers property types and static-array elements the JSON serializer does not represent).

`graph.set_node_settings` (mutation, strict params): `asset_path`, `node_id`, `values` (object: property path -> JSON value), optional `graph_name`, `subgraph_path`, `dry_run` (default false), `expected_fingerprint`. Returns `node_id`, `dry_run`, `changed`, `changes` (`path`, `before`, `after`; `adjusted: true` plus `requested` when the node's own edit reaction changed the value; a value whose JSON exceeds 8192 characters, or the 24000-character budget shared by all values, is `null` with `<field>_omitted_chars`, so a small write that replaces large arrays stays deliverable with its fingerprint), `pins_added`/`pins_removed` (visible pin names; apply only, a preview runs no reaction), `warnings` and the resulting `fingerprint`.

Property paths are relative to the node struct: dot-separated member names, each optionally indexed (`Chains[1].Bone.BoneName`). Traversal enters structs and existing array elements only, never object references (they are other assets).

Refusals change nothing:

| Condition | Code |
|---|---|
| Node is not an anim graph node | `INVALID_OPERATION` |
| Unknown member, member without `CPF_Edit` or with `CPF_EditConst` (also inside an aggregate JSON value), index out of range, traversal through a non-struct | `INVALID_PROPERTY_PATH` |
| Top-level member takes its value from a pin or binding (see above) | `INVALID_OPERATION`, names `graph.set_pin_value` |
| Value holds instanced sub-objects, or is a map or set (their entries cannot be checked like struct fields) | `INVALID_OPERATION` |
| JSON does not convert (failure or any serializer warning); a fixed-size (`EditFixedSize`) array value of another length; a changed number outside its `ClampMin`/`ClampMax` metadata (the details panel enforces it and node reactions rely on it, e.g. `AnimNode_Trail::ChainLength` sizes arrays) | `INVALID_VALUE` (conversion failures carry `details.warnings`) |
| Two paths address the same value (one is the other or a prefix of it, member names compared canonically) | `INVALID_FIELD` |
| A written `FBoneReference` names a bone missing from the target skeleton | `INVALID_VALUE` |
| A non-`None` bone is written to a non-template Anim Blueprint without a target skeleton | `INVALID_OPERATION` |
| `expected_fingerprint` differs, including `node_settings_hash` | `STALE_PRECONDITION` with `details.current_fingerprint` |
| Graph not mutable / blocked asset | existing graph mutator errors |

## Selected design

New `FCortexGraphNodeSettingsOps` (`Private/Operations/CortexGraphNodeSettingsOps.{h,cpp}`), routed by `FCortexGraphCommandHandler`.

Write algorithm:

1. Resolve graph and node with the existing helpers; require an anim node and find its node struct property.
2. Check `expected_fingerprint`: the shared asset fingerprint comparison plus `node_settings_hash`.
3. Copy the node struct to a scratch value. Resolve, preflight and convert every path on the scratch value; validate each written `FBoneReference` (the outermost `FBoneReference` on the path, or every one inside the written value) against `TargetSkeleton` immediately, since a later path may reallocate it. `None` is allowed. A template Anim Blueprint has no skeleton: the write proceeds and a warning says names were not validated.
4. Diff the scratch value against the live struct at the granularity the details panel notifies (innermost changed member, each changed array element separately; an array whose length changed), recording each leaf's property chain and element indices; check numeric clamp metadata on the changed values. No change, or `dry_run`, returns here without a transaction or dirtying.
5. One `FScopedTransaction`: set `RF_Transactional` and `Modify()` on the node and on every node linked to it, record the `Binding` sub-object (`MarkBindingModified`), `PreEditChange(chain)` per leaf, copy the scratch value in, then `PostEditChangeChainProperty` per leaf with the chain (node struct property as member, leaf active) and its array indices — the events the details panel sends, so node-class reactions (alpha link breaking, pin visibility, chain rebuilds) run, also ones handled in the chain hook. Then `Schema->ReconstructNode(*Node)` unless a reaction already reconstructed (the pin objects changed); K2 reconstruction keeps links on pins that still exist. Notify the graph, mark the Blueprint modified.
6. Read back every path from the live node; flag values a reaction adjusted; report the visible pin diff.

Rejected: writing the struct and calling only `ReconstructNode` (skips node-class edit reactions such as the alpha-pin link breaks); text `ImportText` paths (the fragile route the issue replaces); following object references in paths (mutates other assets); a standalone reconstruct command (maintainer decision).

## Acceptance

1. `get_node_settings` on `AnimGraphNode_ModifyBone` returns `BoneToModify`, `RotationMode`, `AlphaInputType`, `AlphaBoolBlend`; `pin_properties` contains `Alpha`, `Rotation`; `property_path` returns one value.
2. `set_node_settings` writes `BoneToModify.BoneName`, `RotationMode`, `AlphaBoolBlend.BlendInTime` atomically; the read-back matches; one undo restores all.
3. Bone validation with a real skeleton (`SK_Mannequin`): an existing bone is accepted, a missing bone is refused with nothing changed; template Anim Blueprint accepts with a warning.
4. `AlphaInputType` Float -> Bool: `bAlphaBoolEnabled` becomes a visible pin and `Alpha` hidden, without save/reload; a link on `ComponentPose` survives; a bool variable linked to `bAlphaBoolEnabled` compiles cleanly.
5. `dry_run` previews before/after and changes nothing, including dirty state.
6. Refusals in the table above change nothing.
7. Array element paths (`[N]`) read and write; out of range is refused.
8. Live editor: the MCP route edits a node; the change and pins survive save and reload.
