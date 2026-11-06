# Issue 173 console dispatch verification

Issue: https://github.com/etelyatn/UnrealCortex/issues/173. Reporter: @mavka-games.

## Candidate and environment

- Plugin base: `30490da05101d12c9a38a54eb3a7062991184036`.
- Reviewed/tested implementation commit: `a628313378455e4dc72d3de0a973716dc97c3b28`.
- Toolkit base: `37ebc855d937928a98782ba38b8ba611ca468b4b`.
- Reviewed toolkit documentation commit: `4db107509cf901e951a77fb50ba9def63db14db0`.
- Windows x64, Unreal Engine 5.8, MSVC 14.44; rendering enabled.
- Reporter environment macOS 15.8.1 / Apple Silicon was not exercised.
- Parent gitlinks and pre-existing StateTree design document remain outside scope. The unrelated CortexSandboxMirror editor was preserved.

## Design and independent review

Read-only subagent design review approved the bounded existing-operation change but highlighted collision precedence. Selected world Exec first, short-circuiting to console-manager processing only if unhandled. No broader player/viewport command surface, extra API, parameter/schema changes or log-derived callback status.

Independent source/tests/docs candidate review found no critical or important defect. Reviewer confirmed world precedence, short-circuiting, weak-world observation, latent shared-handler lifetime, teardown and exact-argument assertions. Controller independently ran the acceptance below; static review did not substitute for runtime evidence.

## Native RED → GREEN

| Run | Result | Raw warnings | Raw errors | Log |
|---|---|---:|---:|---|
| Unchanged operation with new regression | 1/2 pass, new PIE test fails | 0 | 6 expected RED failure diagnostics | `Saved/TestLogs/AutomationTest_2026-10-07_205022.log` |
| Corrected focused console suite | 2/2 pass | 0 | 0 | `Saved/TestLogs/AutomationTest_2026-10-07_205139.log` |
| Complete rendering `Cortex+` suite | 1741/1741 pass | 0 | 1 deliberately expected bind error; 0 unexpected errors | `Saved/TestLogs/AutomationTest_2026-10-07_205247.log` |

The RED test's native player-controller console callback succeeded. Cortex registered-command dispatch failed with zero callback invocations, no world and no arguments. Existing Exec behavior remained functional. GREEN verified one callback with exact world/arguments and world-Exec collision precedence.

The full-suite raw error is `Failed to bind TCP server on ports 18900-18999` during existing `Cortex.Core.TcpServer.ExclusivePort`. `CortexTcpServerTest.cpp` explicitly registers this exact diagnostic once with `AddExpectedError`; the test passed. Do not describe this as zero raw errors.

Both regression and corrected builds succeeded. UBT invocation used `CortexSandboxEditor Win64 Development`, project path `D:/UnrealProjects/CortexSandbox/CortexSandbox.uproject`, `-WaitMutex -FromMsBuild -NoHotReloadFromIDE -NoLiveCoding`. Native runs used `cli/Testing/RunTests.ps1`; focused timeout 180 seconds, full-suite timeout 600 seconds. Runs were serialized.

## Actual stdio MCP / live PIE

Task-owned editor PID `26372`, TCP port `8743`; MCP SDK `ClientSession` used real stdio transport to the checkout's `cortex_mcp.server`, with `CORTEX_PROJECT_DIR` selecting this project. Unrelated editor occupied port 8742 and was not stopped.

Nonce: `012f17c3781048568f084a17d6348920`.

Observed callback messages:

```text
ConsoleDispatch count=1 world=/Game/Maps/UEDPIE_0_TestMap.TestMap args=[native_012f17c3781048568f084a17d6348920|two words|MiXeD]
ConsoleDispatch count=2 world=/Game/Maps/UEDPIE_0_TestMap.TestMap args=[mcp_012f17c3781048568f084a17d6348920|two words|MiXeD]
```

The native comparison used `unreal.SystemLibrary.execute_console_command` with the active game world. MCP invoked `editor_cmd` / `execute_console_command` once. Assertions verified two nonce-bearing callbacks total, sequential counts, equal PIE world paths matching independently printed game-world identity, and exact argument strings including the quoted token and mixed case.

- Unknown nonce-bearing command returned `CONSOLE_COMMAND_FAILED` and identified the rejected input; no extra fixture callback.
- Existing `TRACETAG Issue173LiveExec` dispatch returned `status: ok`; native regression separately proves its actual world mutation and restoration.
- After `stop_pie`, the same fixture command returned `PIE_NOT_ACTIVE`; subsequent logs contained no new nonce-bearing callback.
- `Saved/Issue173SmokeEvidence.json` contains all MCP requests/results; `Saved/Logs/Issue173Live.log` has zero warning/error diagnostics.
- Task-owned editor exited with code 0 after its own MCP `run_python` called `unreal.SystemLibrary.quit_editor()`.
- Initial `uv run` attempted dependency synchronization and encountered an executable lock; the successful probe used the existing virtualenv Python directly, including its stdio server child, without changing or terminating another process.

## Acceptance disposition

| Criterion | Evidence / disposition |
|---|---|
| Console-manager world-and-args dispatch | Native regression and actual live callback messages above; met |
| At most once / stop after handled input | Native callback count, scoped world-command collision and live sequential count; met |
| Correct PIE world and exact arguments | Pointer equality in native test; live independently printed world plus callback paths, quoted token and case-sensitive arguments; met |
| Existing world Exec | Native trace-tag effect/restoration and collision precedence; met |
| Unknown command / inactive PIE / malformed input | Native assertions and live unknown/stopped refusal; met |
| Dispatch versus callback errors | Engine void delegate `ExecuteIfBound` handling semantics; Cortex never inspects callback logs or retries accepted dispatch. Documentation reviewed; met by source semantics, not an intentional-error runtime probe |
| No duplicate API | Existing operation only; Python, router, capabilities and response schema unchanged |
| Toolkit synchronization | Existing editor command reference updated; no generated skill or tool list change needed |

## Source identity

SHA-256 captured before acceptance and matched after runtime:

| File | SHA-256 |
|---|---|
| `Source/CortexEditor/Private/Operations/CortexEditorUtilityOps.cpp` | `528920a97e73aece1e8d38fb34b70ae8cddfa58a082de7a3224294b049a03834` |
| `Source/CortexEditor/Private/Tests/CortexEditorConsoleDispatchTest.cpp` | `727ead8014ea01914685e1c9356c564bceec5106bbba4f83a99862605dbdd73c` |
| `UnrealCortex.uplugin` | `4ca60a0a539548740e925e6567f9a282a7f7f508bfa6677c622f16cb39c57c1b` |

Publication must retain this candidate's source content and ancestry. This report is documentation-only; final PR heads and confirmed merge revisions are recorded in forge delivery communication.

### Simplicity check

- [x] Think before acting — Issue acceptance, current owner, engine dispatch semantics and independent design review established.
- [x] Simplicity first — Existing operation plus short-circuited console-manager processing; no API expansion or callback-status abstraction.
- [x] Surgical changes — One dispatch expression, test-owned generic fixture and scoped design/implementation/verification/toolkit docs; unrelated work preserved.
- [x] Goal-driven execution — Observed RED/GREEN, full native suite and live stdio MCP callback/refusal evidence above; macOS limitation explicit.
