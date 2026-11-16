# Issue 220 implementation: create-command name and path validation

Design: [2026-11-16-issue220-create-name-path-validation-design.md](2026-11-16-issue220-create-name-path-validation-design.md).

## Changes

- `Source/CortexCore/Public/CortexEditorUtils.h`, `Private/CortexEditorUtils.cpp`: new `FCortexEditorUtils::ValidateNewAssetPackageName(PackageName, OutFilename, OutError)`. It checks `FPackageName::IsValidLongPackageName` on the name as given, then `IsWritableMountedContentPath`, then `TryConvertLongPackageNameToFilename`, and returns the `.uasset` filename.
- `Source/CortexMaterial/Private/Operations/CortexMaterialCollectionOps.cpp` (`CreateCollection`) strips trailing slashes. It and `CortexMaterialAssetOps.cpp` (`CreateInstance`) validate `asset_path/name` before the existence check and save to the validated filename. The non-Try `LongPackageNameToFilename` calls are removed.
- `Source/CortexData/Private/Operations/CortexDataAssetOps.cpp` (`CreateDataAsset`) and `CortexDataTableOps.cpp` (`CreateDataTable`) do the same with the package name derived from the object path.
- `Source/CortexBlueprint/Private/Operations/CortexBPAssetOps.cpp`: `Create` and `Duplicate` load the existing asset as `UObject` by full object path. A Blueprint returns `BLUEPRINT_ALREADY_EXISTS`; any other class returns `ASSET_ALREADY_EXISTS`.
- `Source/CortexLevel/Private/Operations/CortexLevelComponentOps.cpp` (`AddComponent`): `StaticFindObjectFast(nullptr, Actor, Name)` before the transaction. An explicit name returns `NAME_COLLISION` unless the occupant is a removed component of the same class. The default name falls back to `MakeUniqueObjectName`.
- `Source/CortexUMG/Private/Operations/CortexUMGWidgetAnimationOps.cpp` (`CreateAnimation`): an object outered to the Widget Blueprint under the requested name whose class is not exactly `UWidgetAnimation` returns `NAME_COLLISION`.

## Tests

- `Cortex.Material.CreatePathValidation` (`CortexMaterialCreatePathValidationTest.cpp`)
- `Cortex.Data.CreatePathValidation` (`CortexDataCreatePathValidationTest.cpp`)
- `Cortex.Blueprint.CreateOverOtherAssetClass` (`CortexBPCreateOverOtherAssetClassTest.cpp`)
- `Cortex.Level.Component.AddComponentNameCollision` (`CortexLevelComponentTest.cpp`)
- `Cortex.UMG.CreateAnimationObjectNameCollision` (`CortexUMGCreateAnimationTest.cpp`)

All drive the public command handlers or router.

## Behavior changes visible to callers

- The four asset create commands return `INVALID_FIELD` for an invalid, unmounted, read-only (`/Engine`, engine and marketplace plugins) or unmappable path. Previously these crashed the editor or wrote into the engine install. `material.create_collection` accepts a trailing slash.
- `blueprint.create`/`duplicate` onto a non-Blueprint asset return `ASSET_ALREADY_EXISTS`.
- `level.add_component` without `name`, repeated with the same class, creates a new uniquely named component instead of re-initialising the first one. An explicit name already in use returns `NAME_COLLISION`.
- `umg.create_animation` returns `NAME_COLLISION` for a name held by another object of the Widget Blueprint.

## Verification

See [../verification/2026-11-16-issue220.md](../verification/2026-11-16-issue220.md).
