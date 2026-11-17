# Issue 227 implementation: MCP server event loop, per-call timeout, handshake id, packaging, dead code

Design: [2026-11-17-issue227-mcp-server-hardening-design.md](2026-11-17-issue227-mcp-server-hardening-design.md).

## Changes

| File | Change |
|---|---|
| `MCP/src/cortex_mcp/tool_output.py` | Sync tools are registered behind an `async` wrapper that runs them through `anyio.to_thread.run_sync`; the `INVOCATION_COMMAND` scope opens and closes inside the worker. |
| `MCP/src/cortex_mcp/tcp_client.py` | `CALL_TIMEOUT` context variable and `_effective_timeout`. `send_command`/`send_command_once` keep failure cleanup, rediscovery and port updates under `_socket_lock`. `_validate_project` uses `_send_and_receive`; `connect()` maps a silent handshake to a retryable `ConnectionError` and keeps `_ProjectMismatchError` distinct. |
| `MCP/src/cortex_mcp/tools/routers.py` | Optional `timeout_s` on the strict router envelope and argument model (validated, clamped to 1–3600 s, set as `CALL_TIMEOUT` for the call). |
| `MCP/src/cortex_mcp/cache.py`, `pagination.py` | One lock per cache around every store access. |
| `MCP/src/cortex_mcp/tools/standalone/qa.py` | Documents the step shape; refuses a malformed `action`/`wait`/`assertion` with `INVALID_FIELD` before sending. |
| `MCP/src/cortex_mcp/tools/standalone/profile.py` | Description names `UMGAuthoring`. |
| `MCP/src/cortex_mcp/tools/{composites,standalone}/*.py` | Removed the `_MCP_ROOT` `sys.path` inserts. |
| `MCP/src/tools/**` | Deleted the uncalled modules (blueprint assets/class_defaults/class_settings/structure/analysis/level_blueprint/migration/search; core assets/asset_deletion/switch_editor; data assets/curve_tables/data_assets/datatables/gameplay_tags/localization; editor pie/viewport/utilities; graph/; level actors/components/discovery/organization/queries/streaming/transforms; material assets/collections/dynamic/graph/parameters; qa actions/assertions/setup/world/detector/reporter; umg widget_animations/widget_properties/widget_tree). Package `__init__.py` files keep only their docstring. |
| `MCP/tools/` | Deleted (dead `editor/composites.py` copy). |
| `MCP/pyproject.toml`, `MCP/uv.lock` | Wheel ships `src/cortex_mcp` and `src/tools`; `mcp>=1.26.0,<2`. |
| `MCP/README.md` | Explicit registration, execution model, `timeout_s`, request ids. |
| `MCP/tests/test_issue227_transport.py` | Event loop, socket ownership, `timeout_s`, handshake regressions. |
| `MCP/tests/test_issue227_tools.py` | `qa_test_step` shape and description. |
| `MCP/tests/*` | Deleted tests that only covered deleted modules; `test_editor_lifecycle.py` now targets the live `tools.editor.composites`; removed the test that pinned the old "skip validation on network error" handshake; the router input-schema contract now includes `timeout_s`. |

## Verification

Offline: `uv run pytest tests/ -m "not e2e and not scenario and not stress"`. Live: a wheel built from the branch, installed in a fresh venv, driven over stdio against a task-owned CortexSandbox editor. Evidence is in [../verification/2026-11-17-issue227.md](../verification/2026-11-17-issue227.md).
