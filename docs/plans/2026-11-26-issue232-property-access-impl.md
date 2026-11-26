# Issue 232 implementation: Property Access nodes

Design: [2026-11-26-issue232-property-access-design.md](2026-11-26-issue232-property-access-design.md).

## Changes

- `Source/CortexGraph/Private/Operations/CortexGraphPropertyAccess.{h,cpp}` (new): `FCortexGraphPropertyAccess`.
  - Class by path (`/Script/PropertyAccessNode.K2Node_PropertyAccess`); `Path` / `TextPath` / `ContextId` by reflection; no link to the PropertyAccessNode plugin.
  - `ParsePathParam` (segment syntax `Name` / `Name[N]`), `ValidateBlueprint` (Anim Blueprint, not a child, skeleton class present, `PropertyAccessEditor` feature loaded), `ValidateGraph` (macro graphs refused), `ValidatePath` (callback-form resolve from the skeleton class; failing segment named; indexed getters, indices on non-arrays, permission-denied members/getters and wildcard leaves refused).
  - `WritePath` / `ReadPath` / `ReadContextId`; `Describe` → `{path, text_path, resolved, thread_safe}`.
- `CortexGraphNodeContract.cpp`: `PropertyAccess` family (aliases `PropertyAccess`, `K2Node_PropertyAccess`, `UK2Node_PropertyAccess`, canonical path; a registry family whose class is not loaded does not resolve), contract (`path` array, required), `Validate`, `ApplyNodeConstructionParams` writes `Path` before pin allocation.
- `CortexGraphNodeOps.cpp`: `property_access` in `SerializeNode` (get_subgraph, traces, handler lists), `search_nodes` entries and the `add_node` response; `add_node` refuses macro graphs; asset-free `describe_node` does not probe this family.
- `CortexGraphPatchOps.cpp`: preflight refuses macro targets; `CompareNodeSymbol` compares path and call site (`context=None`); `path` added to the fail-closed selector list.
- `CortexGraphFingerprint.cpp`: Property Access nodes hash `Path` and `ContextId`.
- `CortexGraphAuthoringContext.cpp`, `CortexGraphCommandHandler.cpp`: `PropertyAccess` published in `supported_families` and the `apply_patch` `nodes` description; `CortexGraphPatchCommandTest.cpp` assertion updated.
- MCP: `tests/fixtures/capabilities_cache_full.json` `apply_patch.nodes` description; `tests/test_graph_authoring_contract.py` `NATIVE_FAMILIES`; `_fallback_generated.py` regenerated (`scripts/sync_fallback.py --from-fixture`, no change: the fallback carries no parameter descriptions).

## Tests

`Source/CortexGraph/Private/Tests/CortexGraphPropertyAccessTest.cpp` (`Cortex.Graph.PropertyAccess.*`) with the test-only native parent `UCortexGraphPropertyAccessAnimInstance` (`CortexGraphPropertyAccessTestTypes.h`, a `float FixedValues[2]` member): see the design's test table.

## Behavior visible to callers

- New `graph` node family `PropertyAccess` for `add_node`, `apply_patch` and `describe_node` (`params.path`).
- Reads of a Property Access node gain `property_access`. Other nodes' responses are unchanged.
- The authoring fingerprint of a Blueprint that contains Property Access nodes changes once (new hashed fields).

## Verification

See [../verification/2026-11-26-issue232.md](../verification/2026-11-26-issue232.md).
