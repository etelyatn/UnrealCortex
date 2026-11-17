# Issue 229 design: self-context getters for inherited members

Issue: https://github.com/etelyatn/UnrealCortex/issues/229.

## Problem

`FCortexGraphNodeContract::ApplyNodeConstructionParams` built every reflected-property `VariableGet`/`VariableSet` with `SetFromProperty(Property, /*bSelfContext=*/false, ContextClass)`. For a member declared on the Blueprint's native parent (for example a C++ `UAnimInstance` subclass) that produced an external member reference with a visible `self` (Target) pin, through `graph.add_node`, `graph.describe_node` probes and `graph.apply_patch`, with or without `owner_class`. Unconnected it fails to compile; wired to a `Self` node it compiles, but an AnimGraph binding loses the fast path. Only Blueprint-declared variables (`NewVariables`, no reflected property yet) and designer widgets were self members.

The editor's My Blueprint getter goes through `UBlueprintVariableNodeSpawner` (UE 5.8 `BlueprintVariableNodeSpawner.cpp:96-110`): self context when `SkeletonGeneratedClass->GetAuthoritativeClass() == OwnerClass || SkeletonGeneratedClass->IsChildOf(OwnerClass)`, which hides the self pin (`K2Node_Variable.cpp:205-208`).

## Decision

- `FCortexGraphSymbolResolver::ResolveProperty` decides the reference form once and records it as `FCortexResolvedSymbol::bSelfContext`, so construction, describe probes and patch readback share it.
  - No owner: the member is found on the Blueprint itself, so it is self context.
  - Declared owner: self context when the Blueprint derives from it (the spawner's rule); external otherwise, unchanged.
- Optional `params.context` keeps the other-instance form the old behavior allowed for an ancestor owner:
  - `"external"` forces an external reference with a visible Target pin.
  - `"self"` is refused with `INVALID_FIELD` for an owner the Blueprint does not derive from.
  - Any other value is refused with `INVALID_FIELD`.
- Construction: `SetFromProperty(Property, bSelfContext, ContextClass)`; property-less members stay `SetSelfMember`, or `SetExternalMember(Name, authoritative class)` when `external` is requested.
- `graph.apply_patch` readback with a declared owner expects `name|self` for a self-context symbol, matching what the native node reports.

## Non-goals

- `CallFunction` already derives self context through `UK2Node_CallFunction::SetFromFunction`.
- No change to rollback replay of removed nodes, which restores the recorded reference.
