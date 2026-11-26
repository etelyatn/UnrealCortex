# Issue 232 design: Property Access nodes (read the path, author new ones)

Issue: https://github.com/etelyatn/UnrealCortex/issues/232.

## Problem

Animation Blueprints read gameplay state through Property Access nodes (`UK2Node_PropertyAccess`, module
`/Script/PropertyAccessNode`, plugin `PropertyAccessNode`, enabled by default, `UncookedOnly`). Cortex can't work with them:

- `graph.describe_node` / `add_node` / `apply_patch` know no `PropertyAccess` family; there is no way to set the path.
- `get_subgraph` / `search_nodes` show the node as "Property Access" plus its `Value` pin, without the path.

The engine class is `MinimalAPI` and its header is private (`Plugins/Developer/PropertyAccessNode/Source/PropertyAccessNode/Private/K2Node_PropertyAccess.h`):
`SetPath`/`GetPath` are not exported and `Path` (`TArray<FString>`), `TextPath` (`FText`) are private `UPROPERTY`s.
CortexGraph must not link the plugin (no plugin dependency for an optional editor plugin), so the class is reached by
path and the fields by reflection.

## Engine facts the design relies on (UE 5.8)

- `UK2Node_PropertyAccess::SetPath` = assign `Path`, `TextPath = IPropertyAccessEditor::MakeTextPath(Path, SkeletonGeneratedClass)`,
  `MarkBlueprintAsStructurallyModified`, `ResolvePropertyAccess()`, `ReconstructNode()`.
- `AllocateDefaultPins` → `AllocatePins` re-resolves `Path` against `Blueprint->SkeletonGeneratedClass`, refreshes `TextPath`
  and creates the `Value` output pin typed from the leaf property (array element when the leaf segment has `[N]`), or a
  wildcard pin when the path does not resolve or the leaf type has no pin type.
- Resolution and thread safety: `IModularFeatures` feature `"PropertyAccessEditor"` (`IPropertyAccessEditor`, public in
  UnrealEd) `ResolvePropertyAccess(Struct, Path, OutProperty, OutArrayIndex)` returns `{Result, bIsThreadSafe}`. The node
  itself stores exactly this `bIsThreadSafe` (`bWasResolvedThreadSafe`) and uses it to decide whether a cached copy is needed.
  Function segments must be getters (one return parameter); the thread-safety flag comes from `BlueprintThreadSafe` metadata
  and object dereferences.
- `IsCompatibleWithGraph` is true only when the Blueprint is a `UAnimBlueprint` (`FPropertyAccessAnimBlueprintBinding`).
- `ExpandNode` fails compilation in a child Anim Blueprint ("currently unsupported in Child Animation Blueprints").

## Design

### Family `PropertyAccess`

- Registry entry in `FCortexGraphNodeContract::ResolveFamily`: aliases `PropertyAccess`, `K2Node_PropertyAccess`,
  `UK2Node_PropertyAccess`, `/Script/PropertyAccessNode.K2Node_PropertyAccess`. The class is found with
  `FindObject<UClass>` on that path; when the plugin is not loaded the family does not resolve (a registry entry with a
  null class is skipped, so `describe_node` reports it unsupported as before).
- Contract: required param `path` (`array`): property-access path segments from the Anim Blueprint class, e.g.
  `["GetOwningActor", "bHidden"]` or `["MyStruct", "Values[2]"]`. Prerequisite text: Anim Blueprints only, not child Anim
  Blueprints. Non-retryable: `INVALID_FIELD, INVALID_OPERATION`.
- `Validate` (asset context, used by `add_node`, `apply_patch` preflight and contextual `describe_node`):
  - Blueprint must be a `UAnimBlueprint` without a parent Anim Blueprint and with a skeleton class (the node's pin
    allocation resolves against `SkeletonGeneratedClass` and the engine resolver asserts on null) → else `INVALID_OPERATION`.
  - `path` must be a non-empty array of strings `Name` or `Name[N]` (N a non-negative integer of at most 9 digits)
    → else `INVALID_FIELD` (`params.path`). The engine segment parser asserts on a trailing `[`, so syntax is checked
    before any segment reaches it.
  - `PropertyAccessEditor` feature unavailable → `INVALID_OPERATION`.
  - Resolve against `SkeletonGeneratedClass` with the per-segment callback form. Refused (`INVALID_FIELD`): an index on
    a getter (the compiler expands the whole return value and never indexes it), an index on a non-array property, an
    index outside a fixed-size C array (the runtime reads `Offset + ElementSize * N` with no bounds check), a
    member or getter denied by the editor permission lists (`UPersonaOptions::IsAllowedProperty`,
    `FBlueprintActionDatabase::IsFunctionAllowed(..., Property)` — the library compiler applies them when Anim Blueprint
    validation is on and turns the failure into a compile *warning*, leaving an unpopulated cache), and a leaf whose pin
    type is missing or wildcard.
  - A failing path names the segment that blocks it: the first failing prefix, or, when the previous segment is a value
    without members (`["bIsDead", "Member"]`), that segment.
- Target graph: macro graphs are refused (`INVALID_OPERATION`) in `add_node`, `apply_patch` preflight and contextual
  `describe_node` with a target. The Anim
  Blueprint compiler creates cached-copy variables only for ubergraph, function and interface graphs; a node inlined from
  a macro with a non-thread-safe path reaches `check(!bRequiresCachedVariable)` in `ExpandNode`.
- `ApplyNodeConstructionParams`: writes `Path` through its `FArrayProperty` (inner `FStrProperty` checked). Nothing else:
  the callers allocate pins afterwards, and `AllocatePins` resolves the path, sets `TextPath` and types the `Value` pin
  exactly as the editor does. No structural mark here, because the same function builds the transient probe nodes of
  `describe_node` and `apply_patch` planning, which must not touch the real Blueprint. A new node adds no member to the
  skeleton class; the generated cache variable is created by the compiler (`CreateClassVariablesFromBlueprint`).
  Asset-free `describe_node` returns the family contract without a probe node (there is no Anim Blueprint class to
  resolve from).
- `apply_patch` readback (`CompareNodeSymbol`): expected = requested segments joined by `.` plus `context=None`
  (Automatic, what Cortex creates); actual = native `Path` plus native `ContextId`. A reused deterministic node whose call
  site was switched therefore fails verification. `path` joins the fail-closed selector list. The graph authoring
  fingerprint hashes `Path` and `ContextId` of Property Access nodes, so an unsaved path or call-site edit invalidates a
  preview token. `apply_patch` preflight also refuses graph-incompatible nodes through `IsCompatibleWithGraph`.
- Published families: `graph.get_authoring_context` `supported_families` and the `apply_patch` `nodes` parameter
  description gain `PropertyAccess` (capabilities fixture and fallback regenerated).

### Readback

`FCortexGraphNodeOps::SerializeNode` (used by `get_subgraph`, `trace_*`, `list_event_handlers`…), `search_nodes` entries and
the `add_node` response add, for Property Access nodes only, in compact and verbose modes:

```json
"property_access": { "path": ["GetOwningActor", "bHidden"], "text_path": "Get Owning Actor.Hidden",
                     "resolved": true, "thread_safe": false }
```

`resolved`/`thread_safe` are computed read-only with `IPropertyAccessEditor::ResolvePropertyAccess` against the
skeleton class: exactly the flag the node records (`bWasResolvedThreadSafe`). `thread_safe` describes the resolved path,
not the compiled call site (an explicit non-Automatic call site can still force a game-thread cached copy); with
`resolved: false` it is `false` and means unknown. A malformed stored path is reported `resolved: false` without reaching
the engine parser. `text_path` is the node's stored display path. `display_name` stays the engine title ("Property Access").

## Out of scope

- Changing the path of an existing node (the proposal asks for creation; remove and re-add the node).
- Call-site / `ContextId` selection (defaults to Automatic, as a node placed from the editor palette).
- Property Access bindings on anim graph node pins (already covered by `set_node_settings` / binding storage work).
- Making `add_node`-created nodes `RF_Transactional` (design review finding 7): pre-existing behavior of `add_node` for
  every node class, not specific to this family; `apply_patch` already creates transactional nodes.
- Picker-only eligibility rules (Blueprint visibility of members): the compiler accepts what the resolver accepts, so
  Cortex follows the resolver plus the permission lists the compiler applies.

## Tests (`Cortex.Graph.PropertyAccess.*`, transient Anim Blueprints through the public router)

| Test | Proves |
|---|---|
| `Describe` | `describe_node PropertyAccess` and the canonical class path are supported with required `path`; asset-free describe with params is family-only without a probe (even for a malformed path); contextual describe returns a typed `Value` pin without adding nodes or dirtying the package; a macro target is refused |
| `AddAndRead` | `add_node` with a member bool path → bool `Value` pin, `property_access` thread-safe; `GetOwningActor.bHidden` → not thread-safe; `get_subgraph` (compact and verbose) and `search_nodes` return the path |
| `Refusals` | Actor Blueprint, child Anim Blueprint, macro graph, missing/empty/non-string/malformed (`bIsDead[`, `bIsDead[x]`) path, unknown segment (named), member of a scalar (names the scalar), index on a non-array, indexed getter, fixed-size array index out of range (`FixedValues[2]` on a test-only native parent with `float FixedValues[2]`; `FixedValues[1]` accepted as a `real` pin), non-getter function; no node added |
| `Patch` | preflight refuses an unknown path and a macro target; `apply_patch` creates a thread-safe access (`bIsDead`, no cache) and the issue's owner read (`GetOwningActor.bHidden` in a thread-safe function, cached copy generated on the class), each wired to a `VariableSet`, compiled with no warnings, readback `matched` |
