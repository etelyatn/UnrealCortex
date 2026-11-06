# Issue 173: PIE console-manager dispatch

Request: https://github.com/etelyatn/UnrealCortex/issues/173, reported by @mavka-games.

## Outcome and boundary

Fix the existing `editor.execute_console_command` operation so a generic `FAutoConsoleCommandWithWorldAndArgs` receives the active PIE world and parsed arguments exactly once. Preserve existing world Exec behavior, unknown-command failures, input validation, and `PIE_NOT_ACTIVE`. No new MCP tool, command, response field, broader player-controller Exec surface, or application-specific dependency.

Success denotes accepted dispatch. A void callback can log an application error after successful dispatch; Cortex must not infer a callback result from log severity or retry a handled command.

## Source basis and selected design

At base `30490da05101d12c9a38a54eb3a7062991184036`, `FCortexEditorUtilityOps::ExecuteConsoleCommand` calls only `GEditor->PlayWorld->Exec`. UE 5.8 `UWorld::Exec` handles a limited world command list and returns false for an unknown command. Native `UConsole::ConsoleCommand` routes via player/viewport and eventually reaches `UEngine::Exec`, whose console-manager processing passes the supplied world. `ProcessUserConsoleInput` tokenizes arguments and returns the bound command delegate's dispatch result, not its application-level outcome.

Retain world Exec first. Only when it returns false, call `IConsoleManager::Get().ProcessUserConsoleInput` with the same command, `*GLog`, and PIE world. A short-circuited Boolean expression ensures no second dispatch after handling. Existing registered objects cannot shadow a previously supported world Exec command. Both unhandled paths retain `CONSOLE_COMMAND_FAILED` and identify the rejected command.

The initial manager-first proposal was independently reviewed. Review found no blocker but highlighted collision precedence; the selected order removes that behavioral expansion. Full in-game controller/viewport routing is intentionally not adopted: it expands the operation beyond preserving the existing world Exec surface plus registered console objects.

## Acceptance

- Generic callback: one invocation, exact active PIE world, case-preserving arguments including a quoted space-containing token; compare against the native player-controller console route.
- Existing world Exec: observable `TRACETAG` mutation, restored afterward.
- Collision: scoped `TRACETAGALL` registration must not receive a callback when world Exec handles it; world flag changes once and is restored. Never replace an existing registration.
- Unknown command: actionable `CONSOLE_COMMAND_FAILED`, no callback effect.
- Active PIE malformed inputs: existing `INVALID_FIELD` behavior.
- Inactive PIE: existing `PIE_NOT_ACTIVE` behavior.
- Live stdio MCP: use the test-owned command with a unique nonce; inspect callback logs, invocation counts, exact arguments, and world identity, not merely the transport response. Confirm stopped-PIE refusal without another callback.

## Ownership and lifecycle

Native fixture lives entirely in the test translation unit under `WITH_DEV_AUTOMATION_TESTS`; its development-editor command unregisters with its automatic console object. Observation uses a weak world reference. Latent test commands own a shared handler through PIE startup, checks, stop, and teardown. No game code or public production test hooks.

## Verification limits

Local acceptance is Windows UE 5.8; the reporter's macOS Apple Silicon environment is not available here. Record actual build, native suite, and MCP evidence separately without claiming a macOS run.
