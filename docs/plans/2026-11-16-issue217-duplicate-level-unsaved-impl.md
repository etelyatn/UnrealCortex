# Issue 217 implementation: duplicate_level unsaved-changes guard

Design: `2026-11-16-issue217-duplicate-level-unsaved-design.md`.

## Changes

- `Source/CortexLevel/Private/Operations/CortexLevelLifecycleOps.cpp`
  - `DuplicateLevel`: after the PIE check, refuse an editor world whose package has no file (`LEVEL_NOT_SAVED`); read `save_current` / `force`; save when requested and the level is dirty; re-check and refuse with `UNSAVED_CHANGES` (`is_dirty: true`) unless `force`.
  - `IsCurrentLevelDirty`: per level (persistent and loaded streaming), also report dirty `GetLoadedExternalObjectPackages()`.
- `Source/CortexLevel/Private/CortexLevelCommandHandler.cpp`: `duplicate_level` declares `save_current` and `force`; the description states the world switch and both refusals.
- `MCP/tests/fixtures/capabilities_cache_full.json` and regenerated `MCP/src/cortex_mcp/_fallback_generated.py` (`uv run python scripts/sync_fallback.py --from-fixture`).

## Tests (`Source/CortexLevel/Private/Tests/CortexLevelLifecycleTest.cpp`)

- `Cortex.Level.Lifecycle.DuplicateLevel` renamed to `...DuplicateLevel.Basic` so the new children do not shadow it (UE runs only leaf test nodes); it passes `force=true` because earlier tests may leave the shared editor world dirty.
- `DuplicateLevel.UnsavedChanges`: dirty map package refusal.
- `DuplicateLevel.UnsavedExternalActor`: transient actor moved to an external package with `SetPackageExternal`, map package clean; refusal. Fixture handles are weak so a regressed command (which reloads the map) cannot crash cleanup.
- `DuplicateLevel.UntitledCurrentLevel`: `NewBlankMap`, `force=true`, `LEVEL_NOT_SAVED`; reloads the original map afterwards.

The refusal tests read `/Game/Maps/TestRoomMap` and never write their destination; cleanup still deletes it in case of regression.

## Verification

See `docs/verification/2026-11-16-issue217.md`.
