# Issue 227: MCP server event loop, per-call timeout, handshake id, packaging, dead code

Request: https://github.com/etelyatn/UnrealCortex/issues/227, reported by @XpycT against `f72a0ad` (0.4.6). #208, #209 and #210 already covered the no-replay half of point 2 and the Gen worker thread; this change covers the rest.

## Outcome and boundary

The Python MCP server only. No native command, response schema or capabilities entry changes. Public surface changes: every `{domain}_cmd` router gains an optional top-level `timeout_s`; `qa_test_step` refuses a malformed step with `INVALID_FIELD`; two tool descriptions gain the missing contract text.

## Source basis (base `606bf439`, 0.4.14)

1. `ToolOutputRegistrar._wrap` registered a plain function for every sync tool. FastMCP 1.26 (`func_metadata.call_fn_with_arg_validation`) calls a non-async function inline, so a blocking `recv` (60 s default, 1800 s for `_LONG_SYNCHRONOUS_COMMANDS`) held the event loop: no pings, no other requests, no cancellation.
2. `send_command` and `send_command_once` called `disconnect()` after leaving `_socket_lock`. With concurrent callers, a failing thread then closed the socket another thread had just opened and sent on; that thread saw a dropped connection and resent its command (RED: `level.get_info` dispatched twice).
3. Routers had no per-call window. A `timeout` inside `params` is a native parameter, not the socket window.
4. `_validate_project` wrote `get_status` without an `id` straight to the socket with a 5 s timeout and swallowed every error. A busy editor skipped the project check, and its late id-less reply passed the id check of the next request.
5. `pyproject.toml` built only `src/cortex_mcp`; `server.py` and the composites import the top-level `tools` package from `src/`. The dependency `mcp>=1.26.0` had no upper bound, and a fresh install resolves mcp 2.x, which removed `mcp.server.fastmcp` (observed while verifying this change).
6. Of the `register_*` functions under `src/tools/`, only the composites, `core/schema`, `data/import_queues` and `reflect/*` are called (sys.setprofile trace of `import cortex_mcp.server`). `MCP/tools/editor/composites.py` and `src/tools/qa/{detector,reporter}.py` were dead copies. Cache invalidation already lives in `UEConnection.send_command` (#211). The `_MCP_ROOT` `sys.path` inserts only served `MCP/tools`.
7. `profile_operation_schema` did not name its only profile; `qa_test_step` did not document the step shape, and a step without `command` answered `Error: 'command'` (an empty command was sent to the editor).

## Selected design

1. **Worker threads.** `_wrap` turns a sync tool into an `async` wrapper that runs it with `anyio.to_thread.run_sync`. The `INVOCATION_COMMAND` scope is opened and read inside the worker (anyio copies the caller's context), so size attribution is unchanged and nothing leaks back to the caller. `ResponseCache` and `PaginationCache` take a lock, because their check-then-act sequences (generation check, snapshot id minting, LRU eviction) are now reachable from concurrent threads.
2. **Lock scope.** `send_command` and `send_command_once` keep every socket mutation, including failure cleanup, rediscovery and the port update, inside `_socket_lock`. The reconnect delay sleeps outside it.
3. **`timeout_s`.** Optional top-level number on the router argument model. Non-numbers, booleans, non-finite values and values `<= 0` refuse with `INVALID_INVOCATION_SHAPE` before dispatch; others clamp to 1–3600 s. The strict wrapper sets an invocation-local `CALL_TIMEOUT` context variable for the call; `send_command`/`send_command_once` use it in place of the per-command window, so every route of the call (batch, cached reads, pagination, graph patch, replay) honours it without threading a parameter through each helper. The handshake keeps its own window. `timeout_s` is never forwarded to the editor.
4. **Handshake.** `_validate_project` sends `get_status` through `_send_and_receive` (request id, default window, normal error path). A silent or dropped handshake fails `connect()` with a plain `ConnectionError` (not `UECommandTimeoutError`: no user command was sent, so the caller's single retry still applies). A project mismatch raises `_ProjectMismatchError` and is never rewritten as "not responding".
5. **Packaging.** The wheel ships `src/cortex_mcp` and `src/tools`; `mcp` is pinned `<2`. Rejected: moving `src/tools` under `cortex_mcp` — it removes the generic top-level `tools` name but rewrites every import and test alias for no behaviour change.
6. **Dead code.** Delete the uncalled modules and aggregate `register_*` functions, `MCP/tools/`, the duplicated QA detector/reporter, the `sys.path` inserts and the tests that only exercised deleted code. Rewrite the README "Tool Discovery" section to describe explicit registration, the execution model and `timeout_s`.
7. **Docs.** Name `UMGAuthoring` in the `profile_operation_schema` description. `qa_test_step` documents `{"command": "qa.<command>", "params": {...}}` for `action`, `wait` and `assertion` and refuses a malformed one with `INVALID_FIELD` before sending anything.

## Acceptance

- A blocking sync tool does not stall the event loop; live pings answer during an 8 s editor call.
- A failing caller never closes another caller's socket; the other command is sent once.
- `timeout_s` sets the window (including replacing a long default), clamps, refuses invalid values before dispatch, applies to one-shot replay dispatch, and is absent from the forwarded params. Live: `timeout_s: 2` times out a 6 s editor block at 2 s; `timeout_s: 90` completes a 65 s block that the 60 s default would cut off.
- The handshake `get_status` carries an id; an unanswered handshake fails the connect as retryable; a mismatch still refuses.
- A wheel installed in a fresh venv starts the server and lists the tools.
- `qa_test_step` refuses a malformed step without sending anything.

## Found, not changed

The deleted `MCP/tools/editor/composites.py` had `_inject_fps_throttle` (`t.MaxFPS 0`, `t.UnfocusedFrameRateLimit 0` after `start_pie`), merged in `a8062fc8` into the dead copy only. The live `editor.start_pie_session` never had it, so no shipped behaviour is lost; whether to port it is tracked separately.
