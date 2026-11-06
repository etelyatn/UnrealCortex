# Issue 180 — Token efficiency, Task 15: one offline Blueprint catalog, lean entry point

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `6e7421c5d998c2a5746821418b04f192996808ed` (Task 4 review fix; current branch HEAD).
The task-15 report recorded its RED stage against `3fcb66d524fe93e2a19f5e815c7fc737a3860341`
(Task 4 implementation commit, parent of this base).
**Owner:** Task 15 implementer (`ImplTask15Catalog`) — RED test staging, the minimal generator change
and this record. All builds/test runs are controller-owned.

## Scope

`_catalog.md` becomes an entry point instead of a second class catalog: it keeps the overview counts
row and gains one relative markdown link to `blueprints.md`, rendered by `render_catalog`. Class
identity — name, parent and asset path — is rendered only in `blueprints.md`. Navigation-only
Markdown change: no sidecar, schema-version or archive change.

Changed files (this task):

| File | Change |
|---|---|
| `MCP/src/cortex_mcp/schema_generator.py` | `render_catalog` blueprint index block: the `### blueprints.md` heading and the `#### Blueprint Classes` `Name`/`Parent` table are replaced by `### Blueprint Classes` plus one deterministic line `[blueprints.md](blueprints.md) — full class catalog: N classes (B blueprints, C cpp).` Row counts are derived from the passed `classes` entries grouped by `type`. The `blueprint_summary` docstring line now states that only counts plus a relative link are rendered here. |
| `MCP/tests/test_schema_blueprint.py` | Staged RED regressions: `test_catalog_links_blueprint_entry_without_class_table`, `test_offline_catalog_links_to_the_blueprint_file`, `test_offline_blueprint_catalog_owns_class_identity`, plus the finite `_make_blueprint_connection`/`generated_blueprint_schema` fixtures; the superseded `test_catalog_includes_blueprint_summary` (which asserted exactly the duplication being removed) is replaced, not re-pinned. |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task15.md` | This record. |

Not part of this commit (deliberately): the parent-repo `docs/systems/cortex-schema.md` update (kept
local for Task 17), and any toolkit change.

## Contract implemented

`render_catalog` (writes `_catalog.md`)

* Schema Overview keeps the counts row `| blueprints | blueprints.md | — | — | — | — | N |` (N =
  number of passed class entries) alongside the data row.
* Schema Index emits `### Blueprint Classes` followed by exactly one line:
  `[blueprints.md](blueprints.md) — full class catalog: N classes (B blueprints, C cpp).`
  No class name, parent or asset path is rendered in the entry point.
* Data index, project/meta sections, the "Always regenerate catalog" call site and
  `_render_meta("catalog", project, engine, plugin)` are unchanged.

`render_blueprint_catalog` (writes `blueprints.md`) is unchanged and remains the authoritative,
deterministic catalog: a flat `| Class | Type | Parent | Asset Path |` table with backticked class
names (for example
``| `BP_CatalogChild_C` | blueprint | ZActor | /Game/Catalog/BP_CatalogChild.BP_CatalogChild |``),
canonically ordered by `_blueprint_class_sort_key`. `collect_blueprint_domain` is unchanged.

Unchanged by construction: the `.meta.json` freshness sidecar semantics (`schema_version`, `domain`,
per-file UTC timestamps), `SCHEMA_VERSION`, the archive JSON, and the fact that both files are
written side by side into one schema directory, so the relative link resolves with no editor
connection.

## Controller-observed RED (before implementation)

From `Plugins/UnrealCortex/MCP`:

```
rtk proxy uv run pytest tests/test_schema_generator.py tests/test_schema_blueprint.py -v
```

Result: **3 failed, 108 passed** — the three staged tests in `tests/test_schema_blueprint.py`. The
Blueprint branch emitted no markdown link and rendered every class in the `#### Blueprint Classes`
`| Name | Parent |` table, so the generated `_catalog.md` duplicated the class names that
`blueprints.md` already owns. No other test in either file failed.

## Controller-observed GREEN (after implementation)

Same command, same files:

```
rtk proxy uv run pytest tests/test_schema_generator.py tests/test_schema_blueprint.py -v
```

Result: **111 passed in 0.71s** (the 108 previously-passing tests plus the three staged tests), no
failures. Both generation-level tests call the real `generate_schema(domain="blueprints")` against a
`pytest` `tmp_path` schema directory and read the linked file and the `.meta.json` freshness data
back, so the run is itself the offline read-back evidence; the repository's pre-existing dirty
`.cortex/schema` snapshots are neither read nor written.

## Task 17 handoff (not done here)

* The parent-repo `docs/systems/cortex-schema.md` Architecture Notes were updated in the parent
  worktree (`_catalog.md` is the entry point with a relative link to `blueprints.md`; class identity
  only in `blueprints.md`). It is a parent-local document and stays out of the plugin commit;
  Task 17 owns consolidated docs/toolkit sync.
* Toolkit resources reference `_catalog.md` as an index/entry point only, so no class-table claim
  needs correction there.
* No command surface or tool signature changed, so `MCP/src/cortex_mcp/_fallback_generated.py`,
  `MCP/tests/fixtures/capabilities_cache_full.json` and
  `MCP/tests/fixtures/mcp_tool_signature_snapshot.json` are unaffected.
* The historical dated plans `docs/plans/2026-02-24-cortex-schema-design.md` and
  `docs/plans/2026-02-24-cortex-schema-impl.md` (and the plugin's older verification assets) retain
  the removed `#### Blueprint Classes` snippet intentionally as records of that period; they are
  dated documents, not current contracts.
* The working-tree `.cortex/schema/_catalog.md` is a previously generated user snapshot and was not
  edited; it renders lean on the next `generate_project_schema` run.
* `MCP/tests/test_e2e.py` carries a pre-existing working-tree modification from Task 4 / peer scope;
  it is unrelated and was left untouched.
