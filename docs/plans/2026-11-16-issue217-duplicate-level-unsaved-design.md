# Issue 217: duplicate_level must not discard unsaved edits

Request: https://github.com/etelyatn/UnrealCortex/issues/217, reported by @XpycT.

## Outcome and boundary

`level.duplicate_level` must never silently discard unsaved edits in the open level, and must never leave the editor on the duplicate because the current level cannot be reopened. Keep the existing duplication mechanism, response shape and the other lifecycle commands' contracts. No new command.

## Source basis

At base `4fe5db5e`, `FCortexLevelLifecycleOps::DuplicateLevel` opens the source as a new map with `UEditorLoadingAndSavingUtils::NewMapFromTemplate`, saves it to `dest_path` with `SaveMap`, then reopens the previous level with `LoadMap`. There was no dirty check, so the world swap dropped unsaved edits and undo history, and an Untitled current level (no file) could not be reopened. `open_level` already refuses a dirty current level with `UNSAVED_CHANGES` unless `save_current` or `force` is set.

The shared `IsCurrentLevelDirty` checked only the persistent and loaded streaming level packages. With one file per actor, an edit dirties only the actor's external package, which the reload discards just the same; `open_level` had the same gap.

## Selected design

- Mirror `open_level`: declare optional `save_current` and `force`. A dirty current level returns `UNSAVED_CHANGES` with `is_dirty: true` unless `force=true`. `save_current=true` saves dirty packages first (same non-modal fast save as `open_level`) and the dirty check is repeated, because the fast save skips packages that have no file yet; anything still dirty is refused instead of discarded.
- A current level without a map file returns `LEVEL_NOT_SAVED` regardless of flags: it cannot be reopened, and saving it would need a Save As dialog.
- `IsCurrentLevelDirty` also inspects `ULevel::GetLoadedExternalObjectPackages()` for the persistent and loaded streaming levels. This tightens `open_level` too, which is the intended contract of its guard.

The issue's alternative (duplicate without a world transition, as `create_level` does for templates) was rejected: duplicating the world package with `StaticDuplicateObjectEx` does not copy external actor packages, so World Partition and one-file-per-actor maps would be duplicated without their actors. The template + `SaveMap` path handles them on save-as.

## Acceptance

- Dirty map package: `UNSAVED_CHANGES`, `is_dirty`, editor world unchanged and still dirty, destination not written.
- Dirty external actor package only: `UNSAVED_CHANGES`, editor world unchanged.
- Untitled current level, even with `force=true`: `LEVEL_NOT_SAVED`, editor world unchanged, destination not written.
- `force=true` duplicates (existing behavior, now explicit).
- Live MCP: a spawned actor in a dirty saved level is refused, then kept on disk after `save_current=true` duplication; `force=true` discards as requested.
