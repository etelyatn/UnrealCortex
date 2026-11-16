# Issue #218 Asset Deletion Safety — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Deleting an asset through Cortex never removes a file whose asset survived, never touches read-only mounts or whole content roots, and refuses referenced assets unless `force`.

**Architecture:** One shared batch-delete and referencer helper in `FCortexEditorUtils` (CortexCore), consumed by `core.delete_asset`, `core.delete_folder`, `blueprint.delete` and `material.delete_collection`.

**Tech Stack:** UE 5.8 C++ (ObjectTools, AssetRegistry), Unreal automation tests, Python MCP fixture.

**Spec:** `docs/plans/2026-11-16-issue218-asset-deletion-safety-design.md`

## Global Constraints

- Stock UE 5.8 engine only; game-thread only; `LogCortex` / domain log categories; zero-warning tests.
- Error codes: `INVALID_FIELD` (missing param, non-writable mount, content root), `ASSET_NOT_FOUND` (missing asset), `HAS_REFERENCES` (registry referencers without `force`), `SERIALIZATION_ERROR` (load failure, engine refused delete, file removal failure).
- Response shapes stay backward compatible; `force` is optional, default `false`.
- RED tests must never target real engine or project content (deleting it is what the bug does).

## Review Focus

1. Engine refuses the delete (`OnAssetsCanDelete` veto, open editor) → file stays on disk, object stays loaded.
2. Folder whose assets reference each other → not blocked by its own internal references.
3. Folder with one externally referenced asset and no `force` → nothing deleted.
4. Path given as object path (`/Game/A/B.B`) → mount check works on the package name.
5. `delete_folder` on `/Game` → refused even though `/Game` is writable.

---

### Task 1: Core shared helper + `core.delete_asset` / `core.delete_folder`

**Files:**
- Modify: `Source/CortexCore/Public/CortexEditorUtils.h`, `Source/CortexCore/Private/CortexEditorUtils.cpp`
- Modify: `Source/CortexCore/Private/Operations/CortexAssetDeletionOps.cpp`, `Source/CortexCore/Private/CortexCoreCommandHandler.cpp` (command info: `force` on both commands, accurate descriptions)
- Test: `Source/CortexCore/Private/Tests/CortexAssetDeletionTest.cpp`

**Interfaces (produces):**
- `static TArray<FString> FCortexEditorUtils::DeleteLoadedAssets(const TArray<UObject*>& Assets);` — result aligned with input; empty string = deleted (object + package file); one `ForceDeleteObjects` call.
- `static TArray<FString> FCortexEditorUtils::GetExternalReferencers(const TArray<FName>& PackageNames);` — sorted unique package names referencing the set from outside it.

Fixture (test-file local): `UObjectLibrary` assets saved under `/Game/Temp/CortexIssue218_<guid>/`, a referencing library holds the target in `Objects`; after save call `AssetRegistry.ScanFilesSynchronous({Filename}, true)` so dependencies are known. Cleanup force-deletes leftovers and removes files/dirs.

- [ ] **Step 1: Write failing tests**
  - `Cortex.Core.AssetDeletion.DeleteAsset.RejectsNonWritableMount`: `/Engine/CortexIssue218Missing/Asset` → `INVALID_FIELD`.
  - `Cortex.Core.AssetDeletion.DeleteFolder.RejectsNonWritableMount`: `/Engine/CortexIssue218Missing` → `INVALID_FIELD`.
  - `Cortex.Core.AssetDeletion.DeleteFolder.RejectsContentRoot`: test root `/CortexIssue218Root` registered via `AddTestWritableContentRoot` → `INVALID_FIELD`.
  - `Cortex.Core.AssetDeletion.DeleteAsset.RefusesReferencedWithoutForce`: B referenced by A → `HAS_REFERENCES`, `details.referencers` contains A's package, B file exists; repeat with `force=true` → success, B file gone. Object path form (`Pkg.Name`) used.
  - `Cortex.Core.AssetDeletion.DeleteAsset.KeepsFileWhenEngineRefuses`: `OnAssetsCanDelete` veto → `SERIALIZATION_ERROR`, file exists, object valid.
  - `Cortex.Core.AssetDeletion.DeleteFolder.BatchesWithInternalReferences`: folder with A→B → success, `deleted_count` 2, both files gone, `OnPreForceDeleteObjects` broadcast exactly once.
  - `Cortex.Core.AssetDeletion.DeleteFolder.RefusesExternalReferencesWithoutForce`: B in folder, A outside → `HAS_REFERENCES`, B file exists.
- [ ] **Step 2: Build; run `Cortex.Core.AssetDeletion+`; expect the 7 new tests to fail for the stated reason** (non-writable/root → wrong code or success; refs → deleted; veto → file gone; batch → 2 broadcasts).
- [ ] **Step 3: Implement helpers and rewire both commands** (validate → load → referencers unless `force` → `DeleteLoadedAssets`). Remove the file-first deletion and the local `TryDeletePackageFile`/`DeleteSingleAsset`.
- [ ] **Step 4: Build; run `Cortex.Core+`; all pass, zero warnings.**
- [ ] **Step 5: Commit** `fix(core): delete asset files only after in-memory delete and guard mounts (#218)`.

### Task 2: `blueprint.delete`

**Files:** Modify `Source/CortexBlueprint/Private/Operations/CortexBPAssetOps.cpp` (`Delete`); Test `Source/CortexBlueprint/Private/Tests/CortexBPDeleteTest.cpp`.
**Consumes:** `DeleteLoadedAssets`.

- [ ] **Step 1:** `Cortex.Blueprint.Delete.KeepsFileWhenEngineRefuses`: create + save a Blueprint, veto via `OnAssetsCanDelete`, `delete` → `SERIALIZATION_ERROR`, `.uasset` exists; cleanup unbinds veto and deletes.
- [ ] **Step 2:** Run; expect fail (file gone).
- [ ] **Step 3:** Replace file-first block + local `ForceDeleteObjects` with `DeleteLoadedAssets`; keep mount check, `force`, `details.references`.
- [ ] **Step 4:** Run `Cortex.Blueprint+`; pass.
- [ ] **Step 5: Commit** `fix(blueprint): remove package file only after in-memory delete (#218)`.

### Task 3: `material.delete_collection`

**Files:** Modify `Source/CortexMaterial/Private/Operations/CortexMaterialCollectionOps.cpp` (`DeleteCollection`), `Source/CortexMaterial/Private/CortexMaterialCommandHandler.cpp` (command info `force`); Test `Source/CortexMaterial/Private/Tests/CortexMaterialCollectionTest.cpp`.
**Consumes:** both helpers, `IsWritableMountedContentPath`.

- [ ] **Step 1:** Extend `Cortex.Material.Collection.DeleteCollection`: after delete, package file absent and `FPackageName::DoesPackageExist` false and registry has no asset. Add `Cortex.Material.Collection.DeleteCollection.RefusesReferencedWithoutForce` (UObjectLibrary references the MPC) → `HAS_REFERENCES`, file exists.
- [ ] **Step 2:** Run; expect both fail.
- [ ] **Step 3:** Implement: mount check, referencers unless `force`, `DeleteLoadedAssets`; drop the `FScopedTransaction` (delete is not undoable; `ForceDeleteObjects` resets the buffer).
- [ ] **Step 4:** Run `Cortex.Material+`; pass.
- [ ] **Step 5: Commit** `fix(material): delete_collection removes the asset (#218)`.

### Task 4: MCP surface, docs, release

- [ ] Add `force` to `delete_asset`, `delete_folder`, `delete_collection` in `MCP/tests/fixtures/capabilities_cache_full.json`; `uv run python scripts/sync_fallback.py --from-fixture`; correct docstrings in `MCP/src/tools/core/asset_deletion.py` and `MCP/src/tools/material/collections.py` (pass `force`).
- [ ] `uv run pytest tests/ -m "not e2e and not scenario and not stress"` passes.
- [ ] Clang gate `python cli/clang/clang-syntax-check.py` on changed files.
- [ ] Live smoke via `core_cmd`: `/Engine/BasicShapes/Cube` refused; referenced temp asset refused then force-deleted; file state observed on disk.
- [ ] Toolkit sync per `cortex-sync-toolkit` where deletion docs exist.
- [ ] Patch bump 0.4.6 → 0.4.7 (`Version` 27 → 28) in a separate commit.
