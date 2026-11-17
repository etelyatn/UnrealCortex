# Issue 229 implementation: self-context getters for inherited members

Design: [2026-11-17-issue229-inherited-variable-self-context-design.md](2026-11-17-issue229-inherited-variable-self-context-design.md).

## Changes

| File | Change |
|---|---|
| `Source/CortexGraph/Private/Operations/CortexGraphSymbolResolver.h` | `FCortexResolvedSymbol::bSelfContext`; `ResolveProperty` documents the context rule. |
| `Source/CortexGraph/Private/Operations/CortexGraphSymbolResolver.cpp` | `ResolveProperty` validates optional `params.context` (`self`/`external`, else `INVALID_FIELD`), marks bare-name members self context, and marks a declared owner self context when the Blueprint's skeleton class is that class or derives from it. `self` on an unrelated owner is `INVALID_FIELD`. |
| `Source/CortexGraph/Private/Operations/CortexGraphNodeContract.cpp` | `ApplyNodeConstructionParams` passes `bSelfContext` to `SetFromProperty`; a property-less member requested `external` uses `SetExternalMember` with the authoritative class. The `VariableGet`/`VariableSet` contract lists `context`. |
| `Source/CortexGraph/Private/Operations/CortexGraphPatchOps.cpp` | Owner-declared readback expects `name|self` for a self-context symbol. |
| `Source/CortexGraph/Private/Tests/CortexGraphInheritedMemberTestTypes.h` | `UCortexGraphInheritedMemberAnimInstance`: native `UAnimInstance` parent with a private `BlueprintReadOnly` float, the issue's shape. |
| `Source/CortexGraph/Private/Tests/CortexGraphInheritedMemberContextTest.cpp` | `Cortex.Graph.AddNode.InheritedMemberContext`, `Cortex.Graph.Authoring.PatchCommand.InheritedMemberContext`, `Cortex.Graph.AddNode.InheritedMemberAnimFastPath`. |

## Verification

Native: the three new tests, then `Cortex.Graph+`, `Cortex.Blueprint+`, `Cortex.UMG+`, `Cortex.Animation+`. Evidence: [../verification/2026-11-17-issue229.md](../verification/2026-11-17-issue229.md).
