# Issue 180 — Token efficiency, Task 8: lean Data catalog and agent capability inventory

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `17b03bbc` ("Task7 fix: an incomplete comparison is not an equal omission",
last committed parent; the Task 8 change is in the working tree pending the controller's scoped
commit)
**Owner:** Task 8 implementer (`ImplTask8Catalog`) — RED regressions, production implementation,
implementation report and this record; all builds, test runs and smokes are controller-owned.

## Scope

Two related token-efficiency cuts, one native read owner and one Python-owned presentation:

1. `data.get_data_catalog` no longer builds sections the caller did not ask for and no longer
   emits the full per-class asset-path inventory by default.
2. The agent-facing `core_cmd(get_capabilities)` defaults to a names-only projection of the
   authoritative capability document; the native handshake and the persisted capability cache stay
   complete.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexData/Private/Operations/CortexDataTableOps.cpp` | `GetDataCatalog` parses/validates `sections` **before any registry/load work**, gates each of the five section blocks behind a selection predicate (unselected ⇒ no array entry at all), and gates the per-class `asset_paths` inventory behind `include_asset_paths` (default `false`). |
| `Source/CortexData/Private/CortexDataCommandHandler.cpp` | `get_data_catalog` `FCortexCommandInfo` advertises the optional `sections` (array) and `include_asset_paths` (boolean) params. |
| `MCP/src/cortex_mcp/tools/routers.py` | New `_get_capabilities(connection, params)` branch in `make_router`'s core path, plus `_project_capability_names` / `_CAPABILITIES_DETAIL_VALUES`. `detail` is validated and removed before the single native dispatch; the projection runs after the authoritative response. |
| `Source/CortexData/Private/Tests/CortexDataCatalogTest.cpp` | Five consumer regressions under `Cortex.Data.Catalog.*` (staged before production; unchanged by this round). |
| `MCP/tests/test_routers.py`, `MCP/tests/test_capabilities.py` | Names-only projection, default view, full-signature availability, both-facade selection forwarding, and Saved-cache-integrity regressions (staged before production; unchanged by this round). |
| `docs/systems/cortex-data.md`, `docs/systems/cortex-core.md` | Catalog and capability documentation updated in the working tree (root-repo docs; integration is Task 17's). |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task8.md` | This record. |

Not part of this task (deliberately): the generated MCP fallback and capabilities fixtures, the
toolkit resources, and root `docs/systems/*` integration across domains — Task 17 owns those.

## Contract implemented

### `data.get_data_catalog` (native)

* **Validation before work.** `sections` is read and validated before any `IAssetRegistry::Get()`,
  `GetAssets` call or asset load. A present-but-not-array `sections`, a non-string/empty entry, or
  an unknown name returns `FCortexCommandRouter::Error(CortexErrorCodes::InvalidField, …)`
  (`bSuccess=false`, non-empty `ErrorCode` and `ErrorMessage`) with **no** section built.
* **Section selectors** (verbatim from `docs/plans/2026-10-13-token-efficiency-design.md` §3.4):
  `datatables`, `gameplay_tags`, `data_assets`, `string_tables`, `curve_tables`. Omitted or an
  empty array selects **all** sections (unchanged default). Only the selected sections are built;
  an unselected section produces no array entry at all (leaner than an empty array — the staged
  helper treats absent and empty identically). Response field names are unchanged
  (`datatables`, `tag_prefixes`, `data_asset_classes`, `string_tables`, `curve_tables`).
* **`include_asset_paths`** (boolean, default `false`): a `data_asset_classes` entry keeps
  `class_name` / `count` / `example_path` and emits **no** `asset_paths` array. Explicit `true`
  restores the full inventory, where `asset_paths` holds the same canonical object paths and
  `asset_paths.Num() == count` exactly (both derived from the same per-asset loop).
* **Cache key.** The selection travels through the Python facade unchanged
  (`data_cmd` generic dispatch and `core_cmd get_data_catalog` cached dispatch both forward
  `route_params`, and `ResponseCache.make_key(command, params)` keys on them), so a selected
  catalog never aliases the default read.

### `core_cmd get_capabilities` (Python-owned presentation)

* `detail` is validated (`names` default, `full` explicit; any other value refuses with
  `_error: INVALID_FIELD`) and **removed** before the single native `get_capabilities` dispatch —
  the native command has no `detail` parameter and never receives it.
* The projection runs **after** the authoritative native response is received. `names` builds a
  **new** document (domains, versions, descriptions and every command `name` preserved; only each
  command's `params` signature inventory dropped) so the source dict is never mutated in place.
  `full` returns the authoritative data, passed with `qualified_command="get_capabilities"` so an
  oversized full inventory still hits the existing complete-or-refuse `RESPONSE_TOO_LARGE` policy
  whose guidance names `detail=names`.
* **Handshake/cache untouched.** `UEConnection.connect` still issues `get_capabilities` with `{}`,
  and the `Saved/Cortex/capabilities-cache.json` writer is not called by the facade — a projected
  read is never written back, so the persisted signatures remain complete.

## Controller-observed RED (before implementation)

* Native `Cortex.Data.Catalog+`: **3 PASS / 4 FAIL** — `DefaultClassInventory`, `SelectedSections`,
  `DataAssetsSection`, `InvalidSectionRefuses` failed; `IncludeAssetPaths` and the two
  `Cortex.Data.Catalog.NoEngine*` guards passed. Log `025656.log`.
* Python `tests/test_routers.py tests/test_capabilities.py`: **133 PASS / 4 FAIL** — the four
  names-only/facade assertions failed; both forwarding/cache guards passed. Log `025656.log`.

## Controller GREEN (recorded)

* Native `Cortex.Data.Catalog+`: **7 PASS / 7** including the four previously-failing regressions.
  Log `032010.log`.
* Python `tests/test_routers.py tests/test_capabilities.py`: **137 passed**. Log `032010.log`.

## Measured byte reduction (fixture-derived)

Compact-JSON sizes of `MCP/tests/fixtures/capabilities_cache_full.json` and of this projection:

| Document | full (chars) | `detail=names` (chars) | reduction |
|---|---|---|---|
| full fixture, all 11 domains | 117 740 | 24 297 | −79.4 % |
| fixture subset (core+data), used by the staged tests | 19 762 | 3 970 | −79.9 % |

The catalog default view additionally drops each DataAsset class's whole `asset_paths` inventory
(one canonical object path per member asset); the exact byte magnitude is project-dependent and is
asserted structurally by the `IncludeAssetPaths` counter-check (`asset_paths.Num() == count`).

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` and `MCP/tests/fixtures/capabilities_cache_full.json`
  still describe `data.get_data_catalog` without `sections` / `include_asset_paths`; regenerate
  after a live capabilities read.
* Root `docs/systems/cortex-data.md` and `docs/systems/cortex-core.md` are edited locally in the
  working tree; the cross-domain root-doc integration is Task 17's.
* Toolkit `resources/{mcp-tool-reference,data-balancing}.md` still describe `get_data_catalog`
  without the selectors and `get_capabilities` without `detail`; Task 17 owns the toolkit sync.
* No MCP tool signature changes: `data_cmd`/`core_cmd` remain `(command, params)`, so
  `MCP/tests/fixtures/mcp_tool_signature_snapshot.json` is unaffected.

## Non-claims

No build, test, formatter, lint or git command was run by this task; the RED and GREEN results
above are the controller's observations. Nothing was suppressed, and no staged assertion was
weakened.
