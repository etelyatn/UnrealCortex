# POC: in-editor MCP-over-HTTP transport (no Python bridge)

**Status: proven end-to-end on UE 5.6.1 (2026-06-24); rebased onto upstream main 0.3.5+ and re-verified
on UE 5.8.2 Linux (2026-10-06).**

Inspired by UE 5.8's native `ModelContextProtocol` plugin: have the editor speak MCP
**directly over HTTP** instead of the current `Python (FastMCP) ⇄ TCP + CortexPort-*.txt ⇄ C++`
chain.

## What was built
`FCortexHttpServer` (`Source/CortexCore/Public|Private/CortexHttpServer.{h,cpp}`):
- Hosts `POST /mcp` via UE `IHttpRouter` (HTTPServer module) on a fixed port.
- Speaks MCP JSON-RPC (MCP "Streamable HTTP", single `application/json` responses):
  - `initialize` → protocolVersion + capabilities + serverInfo
  - `tools/list` → one `<domain>_cmd` tool per registered domain, **generated live** from
    `FCortexCommandRouter::GetRegisteredDomains()` + each handler's `GetSupportedCommands()`
    (command `enum` + params schema).
  - `tools/call` → reconstructs `<ns>.<subcommand>` and dispatches into the **existing**
    `FCortexCommandRouter::Execute(...)`; wraps the result as MCP `content[].text` + `isError`.
  - `ping`, notifications (202).
- Runs **alongside** the TCP server; opt-in via the `cortex.http.port` CVar (default 0 = off;
  e.g. `cortex.http.port=8127` under `[ConsoleVariables]` in the project's `DefaultEngine.ini`). Module wires it in `CortexCoreModule` Startup/Shutdown.

## Verified (live, curl → editor)
- `initialize` → valid handshake.
- `tools/list` → **12 tools**: core/data/graph/blueprint/umg/material/editor/level/qa/reflect/statetree/anim `_cmd`.
- `tools/call` `anim_cmd.list_assets` → real Lyra data (`total_before_limit: 686`), `isError:false`.
- Compile + link clean on UE 5.6.1; isolated (Paradox stays on TCP, untouched).

## Re-verified on UE 5.8.2 Linux (2026-10-07, rebased onto upstream `70ece02d`)
Source-built 5.8.2 engine, blank C++ host project, editor `-nullrhi -unattended`, port 8127, Python probe
over plain HTTP:
- Build: rc=0, 0 errors, all 14 Cortex modules relinked. Upstream `70ece02d` itself does not compile on
  Linux clang (`CortexUMGPropertyBindingOps.cpp`, a range-for over JSON keys); the run applied that
  one-line fix, which is sent separately.
- Default config: no `[http-mcp]` log line, port 8127 not bound, no endpoint file; TCP server unchanged.
- `cortex.http.port=8127` under `[ConsoleVariables]`: endpoint live.
- Auth: no token -> 401, wrong token -> 401, `Origin: http://evil.example` -> 403.
- `initialize` (protocolVersion `2025-06-18`) -> 200, echoes the version, issues `Mcp-Session-Id`;
  `notifications/initialized` -> 202.
- `tools/list` without the session header -> 400; with it -> 200 and **12 tools**, one per registered
  domain (12 `Registered domain` lines in the log).
- `tools/call` `level_cmd.get_info` -> real level data, `isError:false`; an unknown command ->
  `isError:true`.
- `GET /mcp` -> 405 (no SSE stream, see below).

## Key findings
- **The schemas/dispatch were already 100% in C++** — the Python server was a thin bridge, so a
  Python-less HTTP server needed only transport + JSON-RPC framing + a `FCortexCommandInfo`→
  JSON-Schema converter. Confirmed.
- **Editor-environment gotchas (not code)**: `FHttpServerModule` dispatches on the game-thread
  tick. A modal dialog (e.g. "Restore Packages" after an unclean shutdown) or background-tick
  throttling ("Use Less CPU when in Background") starves that tick → requests hang. Any in-editor
  HTTP server inherits this.
- **`MakeError` name clash**: a local helper named `MakeError` collides with UE's global
  `MakeError()` (ValueOrError.h); renamed to `MakeJsonRpcError`.

## Not done (would need productionization)
- **SSE streaming** — responses are single JSON bodies; deferred/long-running tools would need a
  `text/event-stream` path (and the deferred-callback bridge). `run_python defer=true` etc. run
  inline here (nullptr deferred callback).
- **Session lifecycle** — `Mcp-Session-Id` is issued by `initialize` and required afterwards; there is
  no expiry or `DELETE` handling yet.
- **Auth** — a capability token (`X-MCP-Capability-Token`, auto-generated per editor run unless
  `cortex.http.token` pins it, published in `Saved/Cortex/http-endpoint.json`) plus a loopback-only
  `Origin` check. Not reviewed beyond that.
- **5.4.4 build** — ✅ VERIFIED. Built `CortexHostEditor` (minimal blank C++ host) against
  `C:\EpicGames\UnrealEngine-5.4.4`: Succeeded, all Cortex modules incl. CortexHttpServer compiled +
  linked, **0 errors, no version shim** — the `IHttpRouter`/`HttpServer` API is identical enough 5.4↔5.6.
- Real-client connect (Claude Code streamable-http) verified on the 5.6.1 host; not yet wired as the
  Paradox transport (still TCP there).

## Bottom line
The "editor speaks MCP over HTTP directly" approach is **feasible and working** — the Python bridge
and port-file are removable. Remaining work to ship is SSE/server push and the deferred-callback
bridge; neither blocks the concept.
