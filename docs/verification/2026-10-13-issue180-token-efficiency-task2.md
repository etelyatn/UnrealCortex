# Issue 180 — Task 2 verification record (short discovery + optional startup toolsets)

Date: 2026-10-13. Status: implemented; controller-observed RED (§ Controller-observed RED),
GREEN, per-preset stdio smoke, before/after chars and an actual live UI read. Plugin branch
`feat/issue-180-token-efficiency`, base `59bb68352f2df39dc1165a06c585a62fc8faff2f` (the Task-1 state).
The commit identity is recorded in the append-only follow-up docs commit.

## Contract implemented

* `capabilities.resolve_toolset(name, registered_domains) -> frozenset[str] | None`
  (`MCP/src/cortex_mcp/capabilities.py`). `all` → `None` (no filter); `ui`/`blueprint`/`data` →
  the exact design §2.2 membership with registered-domain availability applied; an unknown name
  raises `ValueError` so startup refuses.
* Router descriptions are rendered from **one** metadata command-name list for both the cache
  (`build_router_docstrings`) and no-cache fallback (`minimal_router_docstrings`) paths. The body is
  `Route through <domain>_cmd(command, params). Commands: <names>. Fetch parameters with
  core.get_operation_schema before unfamiliar calls.` with `_COMPOSITE_HINTS` prepended. Parameter
  signatures are gone from the text; the metadata keeps its `params` and is served live by
  `core.get_operation_schema` / `profile_operation_schema`. `_format_command_signature`,
  `_build_fallback_strings` and `_FALLBACK_COMMANDS` were removed (single renderer, no second list).
* `server._register_explicit_tools` loads the capabilities cache, resolves the toolset from
  `CORTEX_TOOLSET` (default `all`) once, and passes it as `ToolOutputRegistrar.allowed_tools`.
  Excluded tools are never registered (Task-1 registrar returns the raw callable for them).
* Composite/standalone/Reflect conciseness: per-parameter prose duplicated by each wrapper's typed
  top-level schema was removed while declarative structure examples, policy, guards, controls,
  examples and outcome guidance were retained. Shortened: `gen_compose`, `statetree_compose`, and 9
  Reflect tools (`scan_project`, `query_class_context`, `get_dependencies`, `query_class_detail`,
  `query_class_hierarchy`, `impact_analysis`, `query_overrides`, `get_referencers`, `query_usages`).
  Short outcome descriptions added to the wrappers that published none: `scenario_compose`,
  `editor_restart`, `profile_operation_schema`, `qa_test_step`, `schema_generate`.
  `blueprint_compose`/`material_compose`/`material_instance_compose`/`widget_compose`/`level_compose`
  are the retained self-sufficient declarative specs (node/pin/widget/operation grammars the typed
  schema cannot express) — evidence recorded in the task-2 report §13.3.
* `MCP/tests/test_composite_docstrings.py` deleted: incidental wording/non-empty assertions only.

## Controller-observed RED (pre-implementation)

Named command `tests/test_capabilities.py tests/test_tool_output.py`: **23 failed, 88 passed in
18.14 s** (Windows `uv run --no-sync`, fresh task-owned basetemp; artifact `artifact://223`).
Failures: missing `resolve_toolset`, rendered parameter signatures in cached/fallback descriptions,
unfiltered `ui`/`blueprint`/`data` surfaces, and an accepted invalid `CORTEX_TOOLSET` that should
refuse startup. 88 preserved passes included the Task-1 invalid `CORTEX_RESPONSE_MODE` startup
refusal and the `allowed_tools` allowlist.

## GREEN command (controller)

From `Plugins/UnrealCortex/MCP`:

```
rtk proxy uv run --no-sync pytest tests/test_capabilities.py tests/test_tool_output.py -v \
  --basetemp=.superpowers/sdd/2026-10-13-token-efficiency-impl/pytest-green-task2
```

Full-file regression (same class of edits): `tests/test_server_registration.py`,
`tests/test_routers.py`, and the broad `tests/ -m "not e2e and not scenario and not stress"`
(`test_composite_docstrings.py` was deleted as an obsolete wording suite).

## Stdio smoke (controller)

`Plugins/UnrealCortex/.superpowers/sdd/2026-10-13-token-efficiency-impl/smoke-task2.py` (task-owned,
disposable). It spawns the shipped server over real stdio with `sys.executable -m cortex_mcp.server`
(`PYTHONPATH` = package `src`, `cwd` = MCP root, a task-owned disposable fixture project) and reports,
per preset: exact tool names, tool count, total description chars, total input-schema chars, a valid
op (`core_cmd schema_status` → `exists=true`), a strict-envelope refusal
(`INVALID_INVOCATION_SHAPE`), and — when `blueprint_compose` is exposed — the description's update
declaration example invoked (non-success, deterministic without an editor) plus the mixed-legacy
refusal (`MIXED_UPDATE_CONTRACT`). Candidate run:

```
rtk proxy uv run --no-sync python ..\.superpowers\sdd\2026-10-13-token-efficiency-impl\smoke-task2.py \
  --label candidate
```

### Baseline mechanism (immutable, no checkout/reset/worktree, no user snapshots)

The controller captured the pre-change tree with `git archive 59bb683 MCP` into the task workspace
(`task2-baseline.zip` → `task2-baseline/MCP`). The same script drives it with the identical fixture
and the default `all` surface:

```
rtk proxy uv run --no-sync python ..\.superpowers\sdd\2026-10-13-token-efficiency-impl\smoke-task2.py \
  --label baseline \
  --mcp-root ..\.superpowers\sdd\2026-10-13-token-efficiency-impl\task2-baseline\MCP \
  --presets all
```

The baseline package resolves ahead of the working tree because the isolated `src` is placed on
`PYTHONPATH` (before the venv's `.pth`-added candidate `src`). The final `SMOKE_JSON` line carries
the machine-readable before/after comparison; no measurement was run by the implementer.

The throwaway scripts (`smoke-task2.py`, the controller's `smoke-task2-live.py`), the task-owned
fixture and the baseline zip/extraction were removed after the measurements below were recorded; the
records in this file are the preserved evidence.

## Observed GREEN, smoke metrics and live read

All of the following were observed by the controller; the implementer ran no test, build, check or
measurement.

* **GREEN**: `tests/test_capabilities.py tests/test_tool_output.py` → **164 passed in 27.22 s**. Full
  staged non-e2e suite → **1019 passed, 265 deselected in 28.36 s** (`artifact://282`). The two
  earlier failures were test-fixture mistakes, fixed without production hacks: the cache lookup reads
  `capabilities["domains"]["core"]["commands"]`, and the preset test asserts `profile_operation_schema`
  only where the exact preset advertises it (the `data` preset omits it) while proving on-demand schema
  discovery through `core_cmd` in every preset.
* **Candidate stdio smoke** (`smoke-task2.py --label candidate`, `artifact://280`):

  | Preset | tools | description chars | schema chars | payload chars |
  |---|---|---|---|---|
  | all | 34 | 33213 | 11963 | 48131 |
  | ui | 11 | 16000 | 3803 | 20873 |
  | blueprint | 21 | 17702 | 6442 | 25766 |
  | data | 6 | 3065 | 1461 | 4872 |

  The valid op, the strict-envelope refusal and the `blueprint_compose` declaration-example/mixed-legacy
  refusals all passed.
* **Before/after** (same fixture, `all` surface; baseline `artifact://255`): description chars
  **56827 → 33213 (−23614, −41.6 %)**; payload chars **72109 → 48131 (−23978, −33.3 %)**; input-schema
  chars **11963 → 11963** (unchanged by design — visibility/description changes never touch the
  published parameter schemas); tool count 34 → 34. Deltas are arithmetic on the two controller
  artifacts; no tokenizer count is claimed.
* **Actual live UI read** (controller's own editor PID `52268`, UE5.8.3; task-owned
  `smoke-task2-live.py`, real stdio with `CORTEX_TOOLSET=ui`): `core_cmd(get_operation_schema,
  umg/get_property)` returned a live contract (`source=live_editor`, full params); `widget_compose`
  created `/Game/Temp/CortexMCPTest/Bench_20261013_193959/WBP_UIPresetStyle` with `verified=true`;
  `blueprint_cmd` `get_info` confirmed `is_compiled=true`; a `data` list read ran; the style read
  `umg_cmd(get_property, ColorAndOpacity)` returned the exact sRGB `#336699` →
  `R 0.033104766 G 0.132868320 B 0.318546772 A 1`, `type=FSlateColor`; the disposable asset was
  deleted (`deleted=true`) and the owned editor shut down normally. The only log diagnostics were
  startup Zen recovery warnings (lines 1063–1064) and shutdown Epic telemetry warnings (2331–2332) —
  no authoring diagnostics — so this is **not** a zero-warning manual-editor claim.
* The parent-repo `docs/systems/python-mcp.md` change is scoped local (not committed with the plugin).

## Review round 1 correction (StateTree declaration grammar)

Independent review (`local://task-2-review.md`, Important P2) found that the shortened
`statetree_compose` description had removed the Python operation-item declaration grammar.
`MCP/src/tools/statetree/composites.py` (docstring-only, +7/−3; no production logic or tests changed)
now restores `{'command': 'add_state', 'params': {...}}` / `{'op': 'add_state', ...}` and states that
explicit `operations` items and the `states`/`transitions`/`removals` shorthands **may be combined in
one call**, matching `_synthesize_operations`.

Covering evidence (controller): `tests/test_statetree_composites.py` → **8/8 passed in 0.04 s**; actual
stdio smoke with `CORTEX_PORT=65000` (unavailable native boundary) showed both restored operation
shapes and a shorthand reaching the native boundary (`isError=true`, opaque `Connection refused at
127.0.0.1:65000`), while a malformed item without `command`/`op` returned JSON `success=false` with
`Operation missing 'command'` before any native dispatch. The first smoke attempt mis-assumed a
JSON-encoded connection exception (`JSONDecodeError`); the controller corrected the script only and
reran to exit 0 in 11.25 s. Deliberately no wording test was added (the existing semantic tests prove
normalization). No native StateTree creation is claimed.

## Commit identity

* `42700f5c849965536d4054b1e9b7cd8f0d3118e1` — "Task2: names-only router descriptions and optional
  startup toolsets" (22 files: `capabilities.py`, `server.py`, the composite/standalone/Reflect
  descriptions, `test_capabilities.py`, `test_tool_output.py`, the deletion of
  `test_composite_docstrings.py`, and this record; parent
  `59bb68352f2df39dc1165a06c585a62fc8faff2f`).
* `2b8e6b2ec3ba6888fbff7d4a34770e632054232a` — "docs: record Task-2 verification commit identity".
* Review round-1 correction commit: the single commit carrying this section and the StateTree
  docstring fix; scoped re-review range `2b8e6b2..HEAD` (SHA recorded by the controller).
* Committed by explicit path only inside the `Plugins/UnrealCortex` submodule; no parent-repo commit,
  no gitlink update, no unrelated path. The parent-repo `docs/systems/python-mcp.md` and the
  git-ignored task workspace (report/ledger/basetemps) stay local.
