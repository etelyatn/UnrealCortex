# Non-modal level saving (issue #148)

## Problem and acceptance

`level.save_level` calls `FEditorFileUtils::SaveLevel` without a filename.
An Untitled editor world can enter Save As, blocking the Game Thread and all
subsequent MCP commands even when the editor was launched with `-unattended`.
Reject a persistent level without an existing map file with `LEVEL_NOT_SAVED`,
explain how to establish a saved map, preserve dirty edits, and allow follow-up
commands. Existing saved maps must still save successfully.

## Design

Keep ownership in `FCortexLevelStreamingOps::SaveLevel`. Check package existence
and the editor's filename before invoking the engine save helper. Return the
domain error with instructions to save through the editor or use
`level.create_level` with an explicit content path (this creates a new map; it
does not preserve the current Untitled map). Do not advertise `save_level_as`,
which Cortex does not implement. Use `UEditorLoadingAndSavingUtils::SavePackages`
with the persistent package and `bOnlyDirty=false`. This uses native non-dialog
save output and preserves map build-data/external-actor saving. `SaveLevel` and
`SaveMap` use dialog-enabled save output even in unattended-script mode. Scope
`GIsRunningUnattendedScript` around the save call using `TGuardValue` as well;
restore the original flag on every return. Retain the existing successful
response and `INVALID_OPERATION` save-failure contract.

## Related paths and scope

UE 5.8 `InternalSavePackagesFast` only saves packages with existing files.
Both `save_all` and `open_level(save_current=true)` use this fast path, so the
issue's suspected Save As hang is not established there. Their skip/failure
reporting behavior is outside this fix. No new save command, domain dependency,
asset format, or generic saving framework is needed.

## Verification

Exercise the real router with a temporary unsaved editor world. In the red run,
set the engine's unattended-script flag in the test to prevent a modal hang;
the old code returns generic `INVALID_OPERATION`, failing the specific error
assertion. After the fix, require `LEVEL_NOT_SAVED`, actionable guidance, retained
dirty state, and a successful follow-up command. Run rendering-enabled Level
automation and the non-live Python suite; inspect raw warning output. A Linux
editor reproduction is not available in this Windows workspace.
