# Issue #218 — Asset deletion safety (design)

Request: https://github.com/etelyatn/UnrealCortex/issues/218 (reported by @XpycT, source review at 0.4.6).

## Problem (confirmed against `origin/main` 4fe5db5e)

1. `core.delete_asset` / `core.delete_folder` (`CortexAssetDeletionOps.cpp`) and `blueprint.delete`
   (`CortexBPAssetOps.cpp`) delete the `.uasset` **before** `ObjectTools::ForceDeleteObjects`. When the
   in-memory delete returns 0 the command fails but the file is already gone; `blueprint.delete` also
   ignores the `IFileManager::Delete` result.
2. `core.delete_asset` / `core.delete_folder` never check the writable-mount policy
   (`FCortexEditorUtils::IsWritableMountedContentPath`), so `/Engine/...` targets and whole content roots
   (`/Game`) are accepted.
3. `core.delete_asset` force-deletes referenced assets (`ForceDeleteObjects` nulls loaded references) and
   reports every failure as `ASSET_NOT_FOUND`.
4. `material.delete_collection` only calls `MarkAsGarbage()`; the file and registry entry survive.
5. `core.delete_folder` calls `ForceDeleteObjects` once per asset (N reference-replace + GC passes).

## Decision

One shared CortexCore deletion path, used by Core, Blueprint and Material:

- `FCortexEditorUtils::DeleteLoadedAssets(const TArray<UObject*>& Assets) -> TArray<FString>`
  (per-asset error, empty on success). One `ForceDeleteObjects` call for the whole batch, guarded by the
  existing `__CortexDeleteGuard__` object. An asset counts as deleted when its weak pointer is gone or
  `RF_Public` was cleared (`ObjectTools::DeleteSingleObject` clears `RF_Standalone | RF_Public`). Only the
  package files of deleted assets are removed afterwards (bounded retry, result checked); a surviving
  asset keeps its file. This is the order `statetree.delete_asset` already uses without the file-watcher
  warning the old comment feared, because the guard object keeps the package non-empty.
- `FCortexEditorUtils::GetExternalReferencers(const TArray<FName>& PackageNames) -> TArray<FString>`:
  asset-registry package referencers of the set, excluding packages inside the set (a folder delete is
  not blocked by references between its own assets).

Command contracts:

| Command | Change |
|---|---|
| `core.delete_asset` | Rejects non-writable mount (`INVALID_FIELD`). New optional `force` (default false): registry referencers → `HAS_REFERENCES` with `details.referencers`, nothing deleted. Load failure / in-memory delete failure / file removal failure → `SERIALIZATION_ERROR` (same as `blueprint.delete`, `statetree.delete_asset`). Missing asset stays `ASSET_NOT_FOUND`. |
| `core.delete_folder` | Rejects non-writable mount and a bare content root such as `/Game` (`INVALID_FIELD`). New optional `force`: any external referencer → `HAS_REFERENCES`, nothing deleted. Deletes all assets with one batch; response shape unchanged (`deleted_count`, `assets`, `failed`). |
| `blueprint.delete` | File removed only after a successful in-memory delete; file-removal failure reported. Existing mount check, `force` and `details.references` unchanged. |
| `material.delete_collection` | Real delete via the shared path: mount check, optional `force` with `HAS_REFERENCES`, file + registry entry removed. |

Error code for "delete refused/failed in engine" reuses `SERIALIZATION_ERROR` to match the two existing
delete commands rather than adding a fourth convention.

## Non-goals

- `statetree.delete_asset` keeps its own (already correct) implementation; migrating it is unrelated.
- No source-control integration (the issue notes bypass; the file-after order no longer deletes files of
  surviving assets, which removes the reported disk/editor divergence).
- `MCP/src/tools/core/asset_deletion.py` and `MCP/src/tools/material/collections.py` are not registered by
  the server; only their docstrings are corrected (issue point 3 cites them). The live contract is the
  C++ `FCortexCommandInfo` surfaced through `core_cmd` / `material_cmd`.
