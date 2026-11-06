# MCP benchmark remediation — verification

**Date:** 2026-10-14
**Source:** `CortexSandbox/Saved/mcp_benchmark_issues_20261014_075611.md` (CortexSandbox project-repo path; 5 findings)
**Design/impl:** `docs/plans/2026-10-14-declared-command-contract-design.md`, `-impl.md`

## Outcome per finding

### 1. `core.get_capabilities` unreadable — not a code defect

The benchmark executed against a **stale MCP server process**: the process serving that session started
2026-10-13 17:32, before the `detail=names` facade commit (2026-10-14 03:34), so it ran pre-facade
Python where `core.get_capabilities` fell through to the generic path and hit the 40 KB guard before
any projection existed.

Evidence on the current build:

- `detail` default and `detail=names` → 29,872 characters, under the budget, with the per-command
  `params` inventory dropped (the projection).
- `detail=bogus` → `INVALID_FIELD: detail must be 'names' (default) or 'full'.`
- `detail=full` → complete-or-refuse (`reader_complete: false`), with no advice to pass a `limit`
  parameter the command does not declare.
- Confirmation of the stale process, observed live after the rebuild: the session's `core_cmd
  get_capabilities` still returns `RESPONSE_TOO_LARGE` with `_size: 158507` — the *old* Python server
  refusing the *new*, larger native payload.

Coverage added: `MCP/tests/test_capabilities_contract.py` (6 tests) pins the ordering invariant (the
projection must run before the size guard), the invalid-`detail` refusal, and that an oversized full
read refuses rather than returning a partial document or inventing a selector.

### 2. `umg.set_padding` reported success without applying the request — fixed

The declared parameter is a nested `padding` object (or a uniform number). The benchmark sent a flat
`top`, which the handler never read, so a zero margin was applied and `set: true` reported. The command
now enforces its declaration.

### 3. `umg.set_anchor` reported success while leaving anchors unchanged — fixed

`anchor` is not a parameter (the anchor is named `preset`), and an empty request silently reported
success. The command now refuses undeclared parameters, an unknown preset token
(`INVALID_PROPERTY_VALUE`) and a request that names no anchor input.

### 4. `material.create_material` path rejected by siblings — withdrawn, not reproducible

Four variants all succeed, including the exact string the report claims fails: the path
`create_material` returns is accepted by `add_node` and `list_nodes` (verified live, sequentially and
in one batch). Consistent with the parallel-invocation race this same benchmark run documented for
`set_transform`; no code change.

### 5. `data.list_data_assets` silently ignored the retired `class_filter` — fixed

`list_data_assets` and `search_assets` now refuse an undeclared selector, so the retired name can no
longer be dropped into an unfiltered, successful-looking result set. The documentation already promised
this ("instead of degrading to an unfiltered project scan"); the code now matches it.

## Live MCP verification (editor pid 40888, port 8742)

| Probe | Result |
|---|---|
| `data.list_data_assets {class_filter: "InputAction"}` | `INVALID_FIELD: Unknown param 'class_filter'. Accepted params: class_name, path_filter` (was: 29 unfiltered assets) |
| `data.list_data_assets {class_name: "InputAction"}` | 4 assets, `resolved_class: "InputAction"` |
| `umg.set_padding {top: 16}` | `INVALID_FIELD: Unknown param 'top'. Accepted params: asset_path, widget_name, padding, target` (was: `set: true` with zeros) |
| `umg.set_padding {padding: {… top: 16 …}}` | `set: true`, echoed `padding {left: 0, top: 16, right: 0, bottom: 0}` |
| `umg.set_anchor {anchor: "Center", alignment: …}` | `INVALID_FIELD: Unknown param 'anchor'. Accepted params: asset_path, widget_name, preset, min, max, alignment, offset` (was: `set: true`, anchors unchanged) |
| `umg.set_anchor {preset: "Center"}` | `set: true`, `anchor {preset: "Center", min: {0.5, 0.5}, max: {0.5, 0.5}}` |

Refusals name the offending parameter and the accepted set, so a caller can correct the call without
reading the source.

## Suite results

Fresh build (exit 0, no new warnings), then: `Cortex.UMG+` 137 completed all `Success`,
`Cortex.UMG.SetterContract+` 1/1, `Cortex.Data+` 131/131, `Cortex.Core+` 205/205, Python non-e2e
1093 passed (baseline 1087 + 6 new).

The single `X` line reported by `Cortex.UMG+` was a latent test left in flight by the runner, not a
failure: the identical artifact appeared in a pre-change run under a *different* test name, no
`Result={Fail}` line exists in the log, and the isolated run of that test passes 1/1.

## Residual risk

- The eight commands that opt in are enforced; the rest of the surface is not (`TD-MCP-005`), including
  `data.search_assets`'s siblings outside CortexData.
- The composite padding shape is covered by unit tests, not live MCP: this session's MCP server process
  predates the change, so the live composite path could not be exercised. A fresh server would.
