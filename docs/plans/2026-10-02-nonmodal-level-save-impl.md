# Non-modal level saving implementation (issue #148)

1. Add `Cortex.Level.Streaming.SaveNeverSavedLevel` before changing production
   behavior. Verify the old command returns the wrong error without blocking.
2. Add `CortexErrorCodes::LevelNotSaved`. Guard the persistent package and editor
   filename in `SaveLevel`, then use native non-dialog `SavePackages` with scoped
   unattended-script mode. Avoid `SaveLevel`'s dialog-enabled save output.
3. Preserve the existing saved-map response. Use an isolated saved-map fixture
   so a locked startup map cannot skip positive coverage. Keep regression test
   names as siblings so Unreal discovers both. Change the remaining save test's
   exclusive-write probe to append mode so it does not truncate map content.
4. Build and run rendering-enabled native Level automation, inspect raw logs,
   and run the non-live Python suite. Record actual results and platform limits
   in `docs/verification/2026-10-02-issue-148.md`.
5. Create a PR, obtain independent subagent review, resolve actionable findings,
   merge after checks, verify issue closure, and remove task-owned temporary
   scripts/branches. Return the plugin and sandbox checkouts to current `main`.
