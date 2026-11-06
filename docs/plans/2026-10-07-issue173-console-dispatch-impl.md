# Issue 173 implementation guide

Design: [console dispatch design](2026-10-07-issue173-console-dispatch-design.md).

## Scoped implementation

1. Add `Source/CortexEditor/Private/Tests/CortexEditorConsoleDispatchTest.cpp`, a real latent PIE regression using a test-only world-and-args console command. Compile and run it against the unchanged operation; confirm missing callback, not fixture setup, is the failing path.
2. Extend `FCortexEditorUtilityOps::ExecuteConsoleCommand` using existing `HAL/IConsoleManager.h`: preserve world Exec first, short-circuit to console-manager processing only when unhandled. Leave input checks, result schema and failure code intact.
3. Build the corrected candidate and run focused console tests, then the complete Cortex automation suite serially with rendering. Independently inspect raw warning/error diagnostics.
4. Launch a task-owned editor with the corrected binary. Use an actual stdio MCP client bound explicitly to this project's port, preserving unrelated editors. Compare native console and MCP callback logs with unique arguments; observe count and world identity. Exercise unknown and stopped-PIE refusals. Shut down only the task-owned editor.
5. Update the existing toolkit editor command reference with PIE requirement, world Exec precedence, console-manager fallback and dispatch-versus-callback distinction. No parameter, capability, Python implementation, or generated skill change is needed.
6. Obtain independent integrated candidate review, verify source-backed findings, and fix blockers before publication. Commit task-owned files in the owning submodules only. Publish ordinary PRs and merge only the reviewed/tested heads under expected-head protection. Verify issue closure and retain branches if ordinary deletion refuses.

## Acceptance ledger

| Criterion | Owner/check |
|---|---|
| Registered callback exactly once, correct PIE world and arguments | Native `Cortex.Editor.Utility.ExecuteConsole.PIE.RegisteredCommandAndExec`; live nonce-bearing stdio MCP callback logs |
| Native console route comparison | Player-controller console dispatch in native regression and live trusted editor Python probe |
| Existing Exec effect and collision precedence | Native trace-tag mutation and scoped registration collision |
| Unknown-command failure and no callback | Native regression and live MCP unknown command |
| Inactive PIE refusal | Existing `Cortex.Editor.Utility.ExecuteConsole.ErrorWhenNoPIE`; live MCP after stop |
| Invalid active-PIE input | Native missing/empty command assertions |
| Dispatch acceptance distinct from application outcome | Bound void-delegate semantics, operation documentation; no log-derived retry or failure classification |
| No duplicate API / schema drift | Existing operation only; no Python/router/capability changes |

Final run paths, exact revisions, review and delivery evidence belong in `docs/verification/2026-10-07-issue173-console-dispatch.md` after execution.
