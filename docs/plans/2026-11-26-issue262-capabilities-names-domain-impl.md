# Issue 262 — capability names view and domain selector implementation

**Design:** `2026-11-26-issue262-capabilities-names-domain-design.md`
**Issue:** https://github.com/etelyatn/UnrealCortex/issues/262
**Owner:** UnrealCortex / Python MCP facade

## Changes

1. `MCP/src/cortex_mcp/tools/routers.py`
   - `_project_capability_names` maps each command record to its `name` string.
   - `_get_capabilities` strips `detail` and `domain` before the single cached native call,
     validates `domain` against the returned `domains` keys (`INVALID_FIELD` listing them), narrows
     the document to that domain, then applies the names projection when `detail=names`. The
     selected `domain` is echoed in `request_params` so an oversize refusal reports it.
2. `MCP/src/cortex_mcp/response.py` — the `get_capabilities` complete-or-refuse guidance names
   `detail=names`, `domain=<name>` and `get_operation_schema`.

## Tests (TDD)

`MCP/tests/test_capabilities_contract.py`:

- names view lists command-name strings;
- the recorded inventory (`tests/fixtures/capabilities_cache_full.json`) names view fits the budget
  (failed before the fix with `RESPONSE_TOO_LARGE`);
- `domain` narrows names and full views; full keeps signatures;
- unknown, empty and non-string `domain` refuse with the registered domains;
- `domain` is not forwarded to the editor;
- the full-read refusal advises `domain`.

Existing names-view assertions in `test_routers.py` and `test_capabilities.py` now expect strings.

## Toolkit

`cortex-toolkit/scripts/sync-cortex-mcp-surface.py` records `domain` beside `detail` as a
Python-owned `core.get_capabilities` parameter; the surface fixture and the MCP reference docs are
updated accordingly.
