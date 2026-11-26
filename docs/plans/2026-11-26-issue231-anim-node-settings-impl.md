# Issue 231 implementation: anim node settings read/write

Design: [2026-11-26-issue231-anim-node-settings-design.md](2026-11-26-issue231-anim-node-settings-design.md).

## Changes

- `Source/CortexGraph/Private/Operations/CortexGraphNodeSettingsOps.{h,cpp}` (new): `FCortexGraphNodeSettingsOps::GetNodeSettings` / `SetNodeSettings`.
  - Path resolution (`Name` / `Name[N]` segments, editable members only, structs and existing array elements only, canonical path for overlap refusal), aggregate-value preflight (nested edit flags, `EditFixedSize` lengths, instanced sub-objects, maps and sets), pin/optional-pin/binding ownership check, immediate per-path `FBoneReference` validation against `UAnimBlueprint::TargetSkeleton` (`bIsTemplate` decides warn versus refuse when there is no skeleton), `ClampMin`/`ClampMax` checks on changed numbers.
  - Apply: scratch `FStructOnScope` copy → leaf diff with property chains and element indices → one `FScopedTransaction` that records the node, its linked nodes (`RF_Transactional` set first) and the `Binding` sub-object, then `PreEditChange(chain)` / copy / `PostEditChangeChainProperty` per changed leaf (member = node struct property) / `ReconstructNode` unless a reaction already reconstructed. Change values over 8192 JSON characters, or beyond a 24000-character budget shared by all values, are omitted with their size.
  - Fingerprint = `MakeObjectAssetFingerprint` (as `graph.set_pin_value`) plus `node_settings_hash` (MD5 of the UTF-8 JSON of the editable settings followed by the struct's exported text).
- `Source/CortexGraph/Private/Operations/CortexGraphNodeOps.{h,cpp}`: `ValidateWritableAssetPath` and `ResolveMutableGraph` expose the existing file-local helpers every graph mutator uses, so the new write route applies the same writable-root and mutable-graph policy.
- `Source/CortexGraph/Private/CortexGraphCommandHandler.cpp`: routes `get_node_settings` (rollback-safe) and `set_node_settings` (behind the blocked-asset guard), both `StrictParams`.
- `MCP/tests/fixtures/capabilities_cache_full.json` mirrors the two native declarations; `MCP/src/cortex_mcp/_fallback_generated.py` regenerated with `scripts/sync_fallback.py --from-fixture`.

## Tests

`Source/CortexGraph/Private/Tests/CortexGraphNodeSettingsTest.cpp`, through the public router on transient Anim Blueprints (nodes created with `graph.add_node`):

| Test | Proves |
|---|---|
| `Cortex.Graph.NodeSettings.Read` | ModifyBone settings members, `pin_properties` (`Alpha`, `Rotation`, not `BoneToModify`), `property_path`, unknown path refusal |
| `Cortex.Graph.NodeSettings.Write` | three paths in one request, read-back, template warning, one undo restores all |
| `Cortex.Graph.NodeSettings.BoneValidation` | `SK_Mannequin` skeleton: existing bone accepted without warnings; missing bone (by path and as a whole `FBoneReference`) refused, atomic, package not dirtied |
| `Cortex.Graph.NodeSettings.Reconstruct` | `AlphaInputType` Bool: `bAlphaBoolEnabled` visible, `Alpha` hidden, pin diff reported, pose links kept; a bool variable linked to `bAlphaBoolEnabled` compiles cleanly into a property-access handler |
| `Cortex.Graph.NodeSettings.DryRun` | preview before/after, node, pins and dirty state unchanged, no pin diff claimed |
| `Cortex.Graph.NodeSettings.Refusals` | each refusal code; overlapping paths; atomicity; current fingerprint accepted; tokens from before an unsaved edit, including a sub-precision float edit, refused |
| `Cortex.Graph.NodeSettings.ArrayElements` | LayeredBoneBlend `LayerSetup[0].BranchFilters` write, element member write, read-back, out-of-range refusal; clearing a 400-element array succeeds with the oversized `before` omitted |
| `Cortex.Graph.NodeSettings.UndoRestoresLinks` | the alpha reaction breaks the `Alpha` link; one undo restores the value and the link on a Cortex-created node |
| `Cortex.Graph.NodeSettings.AggregateRestrictions` | AnimDynamics read-only `BoundBone` refused by path and inside an aggregate; `EditFixedSize` resize refused |
| `Cortex.Graph.NodeSettings.NumericClamps` | Trail `ChainLength: -1` (below `ClampMin=2`, fatal in the node's reaction) refused in preview and apply; an in-range value written |

## Behavior visible to callers

Two new `graph_cmd` commands. Existing commands are unchanged. A Cortex-created node edited by `set_node_settings` (and any node linked to it) becomes `RF_Transactional`, as editor-placed nodes are.

## Verification

See [../verification/2026-11-26-issue231.md](../verification/2026-11-26-issue231.md).
