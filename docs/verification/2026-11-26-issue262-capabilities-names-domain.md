# Issue 262 — capability names view and domain selector verification

**Issue:** https://github.com/etelyatn/UnrealCortex/issues/262
**Branch:** `fix/262-capabilities-names-domain`
**Base:** `2f84af230bc96f1c63c4b58c51bea108ce15254a` (`main` after PR #272)
**Date:** 2026-11-26
**Environment:** Windows 11, UE 5.8 (CortexSandboxMirror editor, PID 56984, port 8742)

Python-only change (`MCP/src/cortex_mcp/tools/routers.py`, `MCP/src/cortex_mcp/response.py`); no
native code changed. See `docs/plans/2026-11-26-issue262-capabilities-names-domain-*.md`.

## Unit evidence

| Stage | Command | Result |
|---|---|---|
| RED | `uv run pytest tests/test_capabilities_contract.py` | 10 failed, 4 passed — recorded-inventory names view refused `RESPONSE_TOO_LARGE`; names entries were objects; `domain` was forwarded to the editor and not applied; no refusal for unknown domain; full refusal did not mention `domain` |
| GREEN | `uv run pytest tests/test_capabilities_contract.py tests/test_routers.py tests/test_capabilities.py tests/test_pagination_contract.py` | 277 passed |
| Suite | `uv run pytest tests/ -m "not e2e and not scenario and not stress"` | 1305 passed, 265 deselected |

## Live editor evidence

Through `strict_router_tool(make_router("core", UEConnection(port=8742)), "core")` against the
running CortexSandbox editor (13 domains, native full document 230,057 compact characters):

| Request | Response chars | Result |
|---|---|---|
| `{}` | 6,904 | all 13 domains, command-name strings |
| `{"detail": "names"}` | 6,904 | same as default |
| `{"detail": "full"}` | 367 | `RESPONSE_TOO_LARGE`, guidance names `detail=names`, `domain`, `get_operation_schema` |
| `{"domain": "material"}` | 901 | `material` only, 41 command names (`list_materials`, `get_material`, …) |
| `{"domain": "material", "detail": "full"}` | 17,193 | `material` only, full signatures |
| `{"domain": "blueprint", "detail": "full"}` | 388 | `RESPONSE_TOO_LARGE` (one domain over budget; per-operation route is `get_operation_schema`) |
| `{"domain": "nope"}` | 204 | `INVALID_FIELD`, lists the 13 registered domains |

## Limits

- A single domain's full inventory can exceed the budget (`blueprint`); it refuses complete-or-
  nothing by design.
