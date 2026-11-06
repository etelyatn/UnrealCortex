# Issue 180 / Token efficiency — Task 1 public output and size metrics

## Candidate identity

- Plan: `docs/plans/2026-10-13-token-efficiency-impl.md` Task 1; design §2.1, acceptance A1.
- Plugin base: `9f899fafeb18666cc8204f7446120d411491861f` (branch `feat/issue-180-token-efficiency`).
- Scope: Python MCP presentation boundary only. No native/C++ change, no editor runtime, no
  description/toolset change (Task 2), no pagination-ownership change (Task 3).
- Tested source identity: the working tree on branch `feat/issue-180-token-efficiency` (plugin base
  `9f899fafeb18666cc8204f7446120d411491861f`), exercised by the controller as GREEN (§GREEN) and by
  the stdio smoke (§Stdio smoke).
- Committed as `67ad840dcb3163c134cdfec5cbeb85dfbac2f960` — "Task1: one public output representation
  and honest response-size metrics" — whose parent is the plugin base above. This verification record
  is committed immediately after that code commit, so the SHA is not duplicated inside it.

## Contract implemented

| Item | Contract |
|---|---|
| `compact_json(value)` (`response.py`) | `json.dumps(value, separators=(",", ":"), ensure_ascii=False)`; the single emitted and budget-counted representation |
| `ToolOutputRegistrar(mcp, connection, response_mode="text", allowed_tools=None, metrics=False)` (`tool_output.py`) | Registration facade for every public tool; `structured_output=False`; sync/async wrappers via `functools.wraps`; delegates `_tool_manager` for the strict-router argument model |
| `text` (default) | exactly one compact JSON `TextContent`; `structuredContent` is `None`; no `outputSchema` and no `{"result": …}` wrapper |
| `structured` | the same compact text plus the decoded object under `structuredContent` |
| Opaque failures | non-JSON strings stay one verbatim text block; never an empty success or inferred native code |
| Explicit `CallToolResult` | passed through unchanged |
| `_meta.cortex` (`CORTEX_RESPONSE_METRICS=1`) | `json_chars` = `len(text)`, `utf8_bytes` = `len(text.encode("utf-8"))`, excluding telemetry itself, once per public return |
| `UEConnection.record_response_size(tool_name, qualified_command, json_chars, utf8_bytes)` + `get_call_metrics()["response_sizes"]` | `{"total": …, "by_tool": …, "by_command": …}`, each `{"json_chars", "utf8_bytes", "calls"}`; `by_command` only when the qualified command is known |
| Budget | `MAX_RESPONSE_CHARS = 40_000` applies to compact JSON text; `response.py` and `graph_patch_boundary.py` both measure/emit `compact_json` |
| SDK floor | `mcp>=1.26.0` in `pyproject.toml` and `uv.lock` metadata |

`CORTEX_RESPONSE_MODE` accepts `text`/`structured` (default `text`); an unknown value refuses
server startup with the valid choices. Hashes, cache keys, durable files, rollback journals
and protocol input serialization were not re-encoded.

## Files

- new: `MCP/src/cortex_mcp/tool_output.py`, `MCP/tests/test_tool_output.py`
- modified: `MCP/src/cortex_mcp/response.py` (compact encoder + all budgets), `graph_patch_boundary.py`
  (`_encode`/`_json_size`), `server.py` (registrar wiring + startup settings), `tcp_client.py`
  (size aggregation), `MCP/pyproject.toml`, `MCP/uv.lock`
- test consumers migrated for the clean cutover: `MCP/tests/conftest.py` (mock client accepts the
  adapter's `CallToolResult`), `MCP/tests/test_e2e.py` (live `profile_operation_schema` consumer)
- test fixtures re-sized to the emitted representation: `MCP/tests/test_response.py`,
  `test_graph_prune_response_boundary.py`, `test_graph_rewire_response_boundary.py`,
  `test_graph_retirement_response_boundary.py`, `test_umg_property_bindings.py`,
  `test_umg_animation_bindings.py`, `test_blueprint_safe_patch.py`

## RED (controller-observed, before implementation)

Combined named command: 4 collection errors — `ImportError: cannot import name 'compact_json'
from 'cortex_mcp.response'` in `test_response.py` and the three graph boundary files.

`tests/test_tool_output.py` against the shipped stdio server (unique `--basetemp`): 14 failed in
11.34 s. Actual observations: default text mode returned
`structuredContent = {"result": "<pretty JSON string>"}`; pretty (multi-line) bodies; the opaque
`Error:` failure wrapped as a `result` string; Reflect `reflect_cache_status` nested string; the
Gen refusal nested string; and the missing `cortex_mcp.tool_output` adapter plus
`UEConnection.record_response_size`.

An earlier run without a unique `--basetemp` produced 6 failures plus 8 `PermissionError` fixture
errors from the default temp location; that is a launcher prerequisite, not product evidence, and
the controller repaired it.

## Compact-encoding evidence (stdlib `json` measurement)

Representative fixtures that must exceed 40,000 characters in the emitted representation:

| Fixture | compact | pretty |
|---|---|---|
| 500-item identity list | 29 186 | 46 194 |
| 800-item identity list | 46 886 | 74 094 |
| Unicode locator `界`×8 000 | 8 227 | 48 306 |
| Unicode locator `界`×41 000 | 41 227 | 246 306 |

This is why truncation fixtures were enlarged: the pretty/escaped form no longer represents the
budget, and the compact form is what a caller receives.

## GREEN (controller-observed)

Round 1 (controller): outer `uv run` failed with `OSError 32` because `cortex-mcp.exe` in the venv
is locked by a running process; rerunning with `uv run --no-sync` collected 125 tests with
116 passed / 9 failed. Of the 9, **8 were stdio children that each died attempting `uv sync`** on the
same lock — a harness prerequisite — and **1 was a genuine fixture budget mismatch**
(`test_apply_refuses_prospective_response_that_exceeds_budget`: the compact prospective envelope
fitted, so the apply proceeded into an unconfigured connection).

Repairs (no product fallback):

* stdio children now run `uv run --no-sync` with `UV_NO_SYNC=1` in the child environment
  (`tests/test_tool_output.py`); the venv already has the editable `cortex_mcp` install and
  `mcp==1.26.0`.
* prospective-budget fixtures now use compact arithmetic: `patch_id="p"*10_000` in
  `test_apply_refuses_prospective_response_that_exceeds_budget` (preview 31 847 ≤ 40 000,
  prospective 41 922 > 40 000) and, from the same source-confirmed defect that the round-1 command
  did not cover, `test_update_oversized_prune_apply_never_mutates`
  (`tests/test_blueprint_safe_patch.py`; preview 31 563, prospective 41 836). Both refusal
  assertions are unchanged.

Round 2 (controller, all Windows `uv run --no-sync`, task-owned fresh basetemp):

| Command | Result |
|---|---|
| 5 named files (`test_tool_output`, `test_response`, prune/rewire/retirement boundary) | **125/125 passed, 7.27 s** |
| UMG property-binding + UMG animation + blueprint safe patch | **101/101 passed, 0.25 s** |
| `tests/ -m "not e2e and not scenario and not stress"` | **1011 passed, 265 deselected, 10.66 s** (`artifact://149`) |

```
rtk proxy uv run --no-sync pytest tests/test_tool_output.py tests/test_response.py tests/test_graph_prune_response_boundary.py tests/test_graph_rewire_response_boundary.py tests/test_graph_retirement_response_boundary.py -v --basetemp=.superpowers/sdd/2026-10-13-token-efficiency-impl/pytest-green-1
rtk proxy uv run --no-sync pytest tests/test_umg_property_bindings.py tests/test_umg_animation_bindings.py tests/test_blueprint_safe_patch.py -v --basetemp=.superpowers/sdd/2026-10-13-token-efficiency-impl/pytest-green-2
rtk proxy uv run --no-sync pytest tests/ -m "not e2e and not scenario and not stress" -v --basetemp=.superpowers/sdd/2026-10-13-token-efficiency-impl/pytest-green-3
```

## Stdio smoke (controller-observed; script removed after proof)

A throwaway, pytest-free script drove the shipped server over real stdio with its own temporary
fixture next to the script (Unicode project name, Gen capability, `.cortex/schema`; auto-removed) and
printed one observable summary line per check. The first run failed to parse because the docstring's
Windows path literal began a truncated `\U` escape; the docstring was made raw (no other literal had a
Windows path or invalid escape). Rerun result: **4/4 passed in 4.74 s**.

| Check | Observed values |
|---|---|
| text mode | `structuredContent=None`, `outputSchema=None`, `newline=False`, schema `exists=True`, Unicode path preserved, standalone `cached=false`, strict extra field → `INVALID_INVOCATION_SHAPE`, opaque `Error:` retained as non-JSON text |
| structured mode | decoded object equals the emitted compact text |
| Unicode metrics | `_meta.cortex` `json_chars=280`, `utf8_bytes=287` |
| optional async Gen | `gen_compose` registered, async refusal returned compact JSON, no job submitted |

The script was deleted after proof; these reports are preserved. Provenance: pytest results and the
smoke values above are the controller's; the compact/pretty fixture arithmetic was computed by me with
the WSL stdlib `json`; my local `py_compile`/`compileall` runs were syntax-only self-checks and are not
build or behavioural acceptance. No native compile/runtime result is claimed.

## Review round 1 — P2 fix (RED and GREEN observed)

Task-1 review (`local://task-1-review.md`) found one Important P2 defect: response-size attribution
used the connection's retained previous command, so a strict refusal (which returns before the router
records an invocation) was reported as a `core.schema_status` response. Consumer regression staged in
`MCP/tests/test_tool_output.py::TestInvocationLocalSizeAttribution` (real `FastMCP`, real
`ToolOutputRegistrar`, shipped router, real `UEConnection` on a temporary port-0 project).

Controller-observed RED (3/3 failed, 0.74 s): `by_command["core.schema_status"]["calls"] == 2`
(expected 1); `by_command["get_status"]["calls"] == 2` (expected 1); concurrent probe
`{"beta": 2}` (expected `{"alpha": 1, "beta": 1}`).

Fix: `tcp_client.INVOCATION_COMMAND` (`contextvars.ContextVar[str]`, default `""` = explicit
unknown/refused state) written by `record_tool_invocation`; the `ToolOutputRegistrar` wrappers open and
reset that scope per invocation (sync and async) and attribute the size to the command routed inside
that invocation; `_last_tool_commands` and `last_command_for_tool` are removed with no compatibility
accessor. `total`/`by_tool` still count every public return; `by_command` only holds invocations that
routed a known command.

```
rtk proxy uv run --no-sync pytest tests/test_tool_output.py -v --basetemp=.superpowers/sdd/2026-10-13-token-efficiency-impl/pytest-green-6
```

GREEN is pending this run; no pass is claimed here.

### Observed GREEN

Covering run `tests/test_tool_output.py` + `tests/test_tcp_client.py`: **57/57 passed in 7.60 s**
(Windows `uv run --no-sync`, task-owned basetemp); the covering set includes the actual stdio
sync/async/strict-refusal/opaque-error cases, so no broad suite rerun was required for this scoped
correction.

Attribution smoke outside pytest: this one ran **in-process** — real `FastMCP.call_tool` (server +
registrar), real `UEConnection` and the shipped core router against a temporary local project — not a
stdio run. (The mode smoke and the composite smoke recorded above *were* actual stdio runs; this label
corrects an earlier overstatement.) It accepted `core_cmd(schema_status)`, then the strict refusal of
`core_cmd` with an unexpected top-level field.

| Observation | Value |
|---|---|
| `by_command["core.schema_status"]` | `json_chars=255`, `utf8_bytes=255`, `calls=1` — unchanged by the refusal |
| `by_tool["core_cmd"]` / `total` | `json_chars=497`, `utf8_bytes=497`, `calls=2` |

So the refusal remains one counted public return (242 characters, derived from 497 − 255) while the
accepted command's bucket does not grow. The smoke script was removed after this record preserved its
evidence. Scoped correction commit: `MCP/src/cortex_mcp/tool_output.py`,
`MCP/src/cortex_mcp/tcp_client.py`, `MCP/tests/test_tool_output.py`, this file.

## Additional controller acceptance

* Existing native decoder runners against unchanged compiled native source: `StreamEvent` 15/15
  (`Saved/TestLogs/AutomationTest_2026-10-13_184301.log`) and `CodexAppServer.Protocol` 10/10
  (`Saved/TestLogs/AutomationTest_2026-10-13_184502.log`), raw severity grep clean
  (no Warning/Error/ensure/fatal). Source branches were inspected for the Claude text-block and Codex
  result-object paths; this is not a new decoder test.
* Real disposable composite in **both** public modes through fresh stdio on a task-owned UE5.8.3
  editor (PID 23672): `/Game/Temp/CortexMCPTest/Bench_20261013_184825/BP_PublicOutput_text` and
  `..._structured` with a `TokenFlag` bool — `success=true`, `variable_count=1`,
  `verification.verified=true`; native `get_info` confirmed `TokenFlag` and `is_compiled=true`;
  compact text, structured decoded equality and metadata sizes verified every call; both assets
  removed via `core.delete_asset` (`deleted=true`); editor shutdown initiated with `force=false`
  (`artifact://168`).
* Honest log classification: that smoke's only diagnostics are shutdown `LogHttp` warnings
  (lines 2399–2400, outstanding Epic telemetry POST); there are no authoring diagnostics, and this is
  **not** presented as a zero-warning manual-editor run.
* Cleanup: the controller's `workspace/smoke-composite.py` is removed once this record preserved the
  evidence; the disposable assets were deleted by the smoke itself.

Coverage still not exercised (no defect asserted): the toolset allowlist path (Task 2) and an invalid
`CORTEX_RESPONSE_MODE` startup refusal.
