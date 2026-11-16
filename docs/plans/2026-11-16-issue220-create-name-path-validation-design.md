# Issue 220: create commands must validate names and package paths before allocating

Request: https://github.com/etelyatn/UnrealCortex/issues/220, reported by @XpycT (found by code reading at `f72a0ad`).

## Outcome and boundary

Create commands must refuse, with a structured error and no side effects, any name or package path that the engine would turn into a fatal error or a silent in-place replacement of another object. Valid requests keep their current results and response shapes. No new command or parameter.

## Source basis

At base `3565f2e9` (0.4.8):

- `material.create_collection`, `material.create_instance`, `data.create_data_asset` and `data.create_datatable` built a package name from caller input and called `CreatePackage` and the non-Try `FPackageName::LongPackageNameToFilename` with no validation. `create_collection` also kept a trailing slash. Engine source (UE 5.8) confirms both failure modes are `Fatal`: `CreatePackage` on a name containing `//` (`UObjectGlobals.cpp`), and `LongPackageNameToFilename` on a path that maps to no mount root (`PackageName.cpp`). A short name (`DA_Test`) makes `CreatePackage` return null. `/Engine/...` was accepted and saved into the engine install.
- `blueprint.create` and `blueprint.duplicate` checked for an existing asset with `LoadObject<UBlueprint>`, so an asset of another class with the same name passed the check and the Blueprint was allocated over it (`StaticAllocateObject` fatal on class mismatch).
- `level.add_component` called `NewObject` with the caller's name, or a class-derived default, with no check. Another class under that name is fatal; the same class is re-initialised in place while both calls report success.
- `umg.create_animation` checked animations, the variable GUID map and widgets, but not other objects outered to the Widget Blueprint, such as its graphs.

## Selected design

- **Package paths:** new `FCortexEditorUtils::ValidateNewAssetPackageName(PackageName, OutFilename, OutError)` in `CortexCore` (the owner of `IsWritableMountedContentPath`). It requires `FPackageName::IsValidLongPackageName` on the name as given, a writable mounted root (`IsWritableMountedContentPath`), and `TryConvertLongPackageNameToFilename`, and returns the filename the caller saves to. The four commands call it before any existence check, transaction or allocation and return `INVALID_FIELD` on refusal. `create_collection` strips trailing slashes like `create_instance`. The material commands validate `asset_path/name` as given, so a `.` in `name` is refused instead of being split into package and object. The Blueprint module keeps its existing equivalent `ValidateWritableBlueprintPath`.
- **Blueprint create/duplicate:** when the package exists, load the asset object by full object path with `UObject`. A Blueprint keeps `BLUEPRINT_ALREADY_EXISTS`; any other class returns `ASSET_ALREADY_EXISTS`.
- **add_component:** look up the name in the actor with `StaticFindObjectFast` (which also finds removed, garbage components). An explicit name held by a live object of any class, or by a removed object of another class, returns `NAME_COLLISION`. A removed component of the same class is reused in place as before, so remove-then-re-add keeps working. When no name is given and the default name is taken, `MakeUniqueObjectName` picks a free one.
- **create_animation:** an object of another class outered to the Widget Blueprint under the requested name returns `NAME_COLLISION`. A same-class `UWidgetAnimation` is reused as before: `remove_animation` keeps the object for undo, and the name must remain reusable.

## Acceptance

- `create_collection` with a trailing slash creates the asset at the normalized path.
- `create_instance` with `Game/Mats`, `create_data_asset` with `DA_Test`, `create_datatable` with `Game/...`, a `//` path, a `.` in the material name, and `/Engine/...` for collections, DataAssets and DataTables are refused with `INVALID_FIELD`, no crash and no package.
- `blueprint.create` and `blueprint.duplicate` onto a DataTable's name return `ASSET_ALREADY_EXISTS` and leave the DataTable intact.
- `level.add_component`: two unnamed adds of one class produce two listed components with distinct names; an explicit name held by another class or the same class returns `NAME_COLLISION` and leaves the component unchanged; after `remove_component`, another class is refused and the same class can re-add the name.
- `umg.create_animation` named `EventGraph` returns `NAME_COLLISION` with no animation or identity added; remove-then-create of one animation name succeeds.
