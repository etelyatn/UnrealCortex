# UnrealCortex MCP Server

Python MCP server that bridges Claude Code (and other MCP clients) to the UnrealCortex C++ plugin running inside Unreal Editor.

## Quick Start

```bash
cd Plugins/UnrealCortex/MCP
uv run python -m cortex_mcp
```

Port is auto-discovered from `Saved/CortexPort-{PID}.txt` (written by CortexCore on editor startup; one file per running editor instance). Override with `CORTEX_PORT=8742` env var.

## Directory Layout

```
MCP/
├── src/cortex_mcp/         # MCP server core (wheel package `cortex_mcp`)
│   ├── server.py           # FastMCP server + explicit tool registration
│   ├── tool_output.py      # Single result-representation adapter for every public tool
│   ├── tcp_client.py       # TCP connection to CortexCore
│   ├── cache.py            # Response caching
│   ├── response.py         # JSON formatting helpers
│   └── tools/              # Routers (`{domain}_cmd`), composites, standalone tools
├── src/tools/              # Composite and Reflect implementations (wheel package `tools`)
└── tests/                  # Unit + E2E tests
```

Both `cortex_mcp` and `tools` ship in the wheel, so a non-editable install works.

## Tool Registration

There is no auto-discovery. `server._register_explicit_tools` registers the public surface through one `ToolOutputRegistrar`:

- one router per registered domain (`cortex_mcp/tools/routers.py`);
- composites (`cortex_mcp/tools/composites/`), which wrap the implementations in `src/tools/{domain}/composites.py`;
- standalone tools (`cortex_mcp/tools/standalone/`, `src/tools/reflect/`, `src/tools/data/import_queues.py`).

A new public tool must be wired into `_register_explicit_tools`; a `register_*` function that nothing calls is dead code.

### Execution Model

The registrar runs every synchronous tool in a worker thread (`anyio.to_thread.run_sync`), so a long editor round trip never blocks the event loop: pings and other requests keep being served. One socket serves all calls; `UEConnection` serializes round trips and every socket mutation under its lock. Shared caches (`ResponseCache`, `PaginationCache`) are thread-safe.

### Per-Call Timeout

Every `{domain}_cmd` router accepts an optional top-level `timeout_s` (seconds, clamped to 1–3600). It replaces the receive window for every editor round trip of that call, including the built-in long windows (for example `blueprint.compile_all`). Without it each command keeps its default window (60 s; longer for known long commands). A timed-out command is never resent.

```json
{"command": "compile", "params": {"asset_path": "/Game/BP_Large"}, "timeout_s": 300}
```

### Command Surface Discipline

Keep domain command surfaces focused on real workflows. Do not add per-domain diagnostic commands when the global built-ins already cover the need:

- `get_status` reports editor/server health.
- `get_capabilities` reports registered domains and command metadata.

Domain commands should do domain work, and composite tools should exist only when they reduce real multi-step workflows. Future placeholder commands or empty wrapper modules should wait until the underlying behavior is implemented and tested.

## TCP Protocol

Commands are line-delimited JSON sent to `127.0.0.1:{port}`:

```json
{"id": "a1b2c3d4", "command": "data.list_datatables", "params": {}}
```

Response:

```json
{"success": true, "data": {...}}
```

Namespace prefix routes to the registered domain handler in C++. Built-in commands (`get_status`, `get_capabilities`) have no prefix. Every request carries an `id`, including the connect handshake (`get_status` project check, then `get_capabilities`), so a late reply cannot be taken as the answer to a later request. An editor that does not answer the handshake fails the connect; no command was sent, so the call retries once.

## Failure shape

Every public tool reports a failure as one object and sets `isError`:

```json
{"success": false, "_error": "INVALID_VALUE", "_message": "location must be a 3-number array", "...": "non-reserved details"}
```

- `ToolOutputRegistrar` sets `isError=true` whenever the decoded result is a dict with a non-empty `_error`; a successful result or a `success:false` outcome without a code is not marked as an error.
- A tool that raises `UECommandError` is published with the editor's `code`/`message` and its non-reserved details; a raised `ConnectionError` becomes `_error: "CONNECTION_ERROR"`. Other exceptions still propagate (programming errors).
- Non-JSON text is never parsed for a code and stays an opaque text block.
- Owners return this object at their source (routers, composites, standalone and Reflect tools): a locally refused request returns `INVALID_FIELD`/`INVALID_OPERATION`, a failed batch step passes the first native `error_code` through, and a lost connection returns `CONNECTION_ERROR`. Do not add a second failure shape or an alias for an old one.

## Testing

```bash
# From a standalone UnrealCortex checkout (use Plugins/UnrealCortex/MCP in a host project)
cd MCP
rtk proxy uv sync --group dev

# Offline tests: no editor or host .uproject required
rtk proxy uv run pytest tests/ -m "not e2e and not scenario and not stress" -v
rtk proxy uv run python scripts/sync_fallback.py --from-fixture --check

# E2E tests (requires running UE editor)
rtk proxy uv run pytest tests/ -m "e2e and not stress" -v
```

Use marker selection (`-m`), rather than test-name selection (`-k`), to exclude
editor tests. Live scenario modules also carry `e2e`, so
`-m "not e2e and not stress"` is an equivalent offline selection today.
An explicit `CORTEX_PROJECT_DIR` selects the project for connected tests; the
connection fixture temporarily supplies the containing workspace only when this
variable is absent and restores it even if discovery or connection fails.

Schema and project-discovery unit tests create temporary project layouts.
Checks of sibling `cortex-toolkit/` resources and CortexSandbox workspace docs
skip with a named prerequisite when those directories are absent. When present,
the checks still fail on missing files or invalid content. Toolkit example
scenarios require `cortex-toolkit/examples/typed-blueprint-authoring/` as well as
a live editor. Other live tests may require CortexSandbox assets and classes;
this offline isolation change does not make them generic host-project tests.

### Blueprint migration cleanup persistence

`cleanup_blueprint_migration` and `blueprint.cleanup_migration` accept strict boolean `compile` and `save`, both defaulting to `true` for existing callers. `save=false` stages cleanup in memory and leaves package bytes on disk unchanged; compilation is independent. The response reports `saved`, `is_dirty`, `compiled` and `compile_status`. A compiler-error result is not saved. Saving writes the entire package, including any edits that were already dirty before cleanup. With `migrated_overrides`, initial cleanup and orphan pruning stay unsaved and uncompiled; final cleanup performs the requested compile/save after all pruning, and its final state is returned with the original mutation inventory.

### UMG widget rename contract

`umg.rename_widget` / `rename_widget` performs a guarded in-memory widget rename with structural and skeleton refresh (`skeleton_regenerated=true`, `saved=false`), leaving package saving and full recompilation to caller orchestration:
- **Identifier discipline:** Requires literal identifier `new_name` (must begin with a letter or underscore, alphanumeric/underscore characters only, non-empty, length below `NAME_SIZE`). Sanitizing, slugging, and case-only renames are rejected.
- **Fingerprint pre-condition:** Requires complete widget tree `expected_fingerprint` containing `compiled_signature_crc`. A mismatching supplied fingerprint returns `StalePrecondition`.
- **Containment safeguards:** Refuses rename if loaded child Blueprints or external Blueprints reference the widget member or any generated UI-component member attached to it (`InvalidOperation`), preventing unguarded external package mutation. Class-instance-only dependencies do not block rename.
- **Binding & animation preservation:** Maintains engine reference-aware rename for widget tree, property bindings, focus, navigation, and child variables, and guarantees all widget animation bindings are updated—including cases where slot bindings precede direct bindings for the same widget. Preserves slot names, GUIDs, MovieScene possessables, tracks, section ranges, and keyed values across the rename.
