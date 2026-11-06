# PR #134: shared editor module ordering

Retain Boshchuk's descriptor-only correction: list CortexEditor before CortexMaterial, keeping both Editor/PostEngineInit and all other entries unchanged.

CortexMaterial declares CortexEditor as a private dependency. UE 5.8 FModuleDescriptor::LoadModulesForPhase iterates the descriptor array in order. This aligns startup ordering with the existing dependency; it does not establish a new architecture or change commands, schemas, or persistence.

Acceptance: cold startup registers editor before material; existing material automation passes. Contributor stack/Linux build claims are not independent maintainer evidence.
