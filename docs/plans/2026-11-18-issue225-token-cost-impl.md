# Issue 225 implementation plan

Design: `2026-11-18-issue225-token-cost-design.md`. Issue: UnrealCortex #225.
Base: plugin `f305ab82` (0.4.16). All Python work under `MCP/`, C++ under `Source/`.
Test runner: `cd MCP && uv run pytest tests/ -m "not e2e and not scenario and not stress" -q` (staged); UE suites via `cli/Testing/RunTests.ps1` with `Cortex.Blueprint+` / `Cortex.Graph+` / `Cortex.Level+`.

## Task 1: composite descriptions (Python)

Files: `MCP/src/cortex_mcp/tools/composites/blueprint.py`, `MCP/src/tools/blueprint/composites.py`, `MCP/src/tools/material/composites.py`, `MCP/src/tools/umg/composites.py`, `MCP/src/cortex_mcp/tools/routers.py` (`_schema_status` suggestion). Tests: `MCP/tests/test_composites.py`.

1. RED: new test `test_registered_descriptions_name_no_unregistered_tool` — register the full default surface offline (FastMCP + fallback capabilities, as `measure_listing_tmp.py` does), then assert no description contains any name from the known-unregistered set (`create_blueprint`, `add_blueprint_variable`, `graph_add_node`, `create_widget_screen`, `create_blueprint_graph`, `create_material_graph`, `create_material`, `add_material_node`, `connect_material_nodes`, `generate_project_schema`, the six reflect wrappers once Task 2 lands). Also assert the `blueprint_compose` description contains `mode='update'` and does not contain `Only "create"`.
2. RED: assert `blueprint_compose` description length <= 4200 and `material_compose` <= 4600, `widget_compose` <= 3200 (current: 7711/4315/3121).
3. GREEN: rewrite `blueprint.py` `_bp_doc` assembly — replace the `_SAFE_UPDATE_DOC + legacy docstring` concatenation with one authored description covering: composite routing sentence; create mode (spec fields, node classes, pin conventions condensed, refused-update legacy note removed since update IS supported); update mode (patch envelope summary, reviewed-patch protocol condensed, refused legacy fields); one short example per mode named `blueprint_compose`. Trim the legacy docstring in `src/tools/blueprint/composites.py` to match (it is also the captured create-mode text). Remove stale chains/examples in material and umg docstrings; fix `routers.py` `_schema_status` suggestion to `schema_generate`.
4. Verify: new tests pass; `test_composites.py`, `test_routers.py` green.

## Task 2: delete six duplicate reflect wrappers (Python)

Files: delete `MCP/src/tools/reflect/{dependencies,referencers,detail,hierarchy,overrides,usages}.py`; modify `MCP/src/tools/reflect/__init__.py`; `MCP/src/cortex_mcp/capabilities.py` (`_REFLECT_STANDALONE_TOOLS`, `_BLUEPRINT_TOOLSET`, comments); tests `MCP/tests/test_reflect_tools.py`, `MCP/tests/test_capabilities.py`.

1. RED: update `test_capabilities.py` — `REFLECT_STANDALONE_TOOL_NAMES` keeps the five; `DEFAULT_TOOLSET_SURFACE` drops the six; add `test_default_surface_has_no_duplicate_reflect_wrappers` asserting resolve+registration never yields the six names.
2. GREEN: delete the six modules and their `register_*` calls; shrink `_REFLECT_STANDALONE_TOOLS` to the five; update the §2.2 preset comment.
3. Tests: delete the six wrappers' tests in `test_reflect_tools.py` (hierarchy/detail/overrides/usages registration + their unit-tested helpers stay only where the module survives); keep cache/context/impact tests. Full staged suite green.

## Task 3: truthful truncation hints (Python)

Files: `MCP/src/cortex_mcp/response.py`, `MCP/src/cortex_mcp/tools/routers.py`. Tests: `MCP/tests/test_response.py`, `MCP/tests/test_routers.py`.

1. RED: `format_response` with an oversized non-paging payload (e.g. tool `blueprint_cmd`, command `get_info`) asserts the `_truncated.suggestion` does not contain `limit` and names narrowing; same for the no-truncatable-array refusal.
2. RED: `format_response(..., pages=True)` keeps the `limit` pagination text.
3. GREEN: add `pages: bool = False` to `format_response`; the three suggestion sites pick the text from it; routers pass `pages=True` on the `CACHED_PAGE_ARRAYS` fallback paths (`_handle_limit_request` missing-array fallback, cached-read and plain dispatch fallbacks).
4. RED: policy entries — oversized `graph.trace_exec`/`graph.trace_dataflow` refuse naming `max_depth`; oversized `query_class_hierarchy` flat read refuses naming `max_results`/`root`.
5. GREEN: add the three entries to `_COMPLETE_OR_REFUSE_POLICIES`.

## Task 4: page 1 + cursor for no-limit CACHED_PAGE_ARRAYS overflow (Python)

Files: `MCP/src/cortex_mcp/tools/routers.py`. Tests: `MCP/tests/test_pagination_contract.py`.

1. RED: fake connection returns an oversized `blueprint.list` response (many blueprints); calling the router without `limit` returns page 1 (first N entries), `pagination.next_cursor` set, `total` correct; calling with that cursor returns the next contiguous slice (no skipped/duplicated rows).
2. RED: a response that fits is returned whole with no cursor; a response whose mapped array is missing falls back to `format_response` (no snapshot crash).
3. GREEN: `_serve_cached_first_page(qualified, data, params)` helper mirroring `_batch_query`'s tail: snapshot full list + template into `_pagination_cache`, binary-search the largest page whose `_format_cached_page` output fits (capped at `_MAX_LIMIT`), return it; wire into both dispatch paths for `CACHED_PAGE_ARRAYS` when no `limit`/`cursor` was passed.

## Task 5: blueprint.get_info inherited default + source (C++)

Files: `Source/CortexBlueprint/Private/Operations/CortexBPAssetOps.cpp` (GetInfo); tests `CortexBPListAndInfoTest.cpp`, `CortexBPCompactSerializationTest.cpp`.

1. RED: `Cortex.Blueprint.GetInfo.IncludeInheritedDefaultFalse` — get_info on an Actor-child BP without `include_inherited` returns only Blueprint-defined functions (no `ReceiveTick` etc.).
2. RED: compact + `include_inherited=true` — every function entry has `source` in {`blueprint`, `inherited`}; own and inherited are distinguishable. Compact default (false) keeps `source` absent (flip the existing pin in `CortexBPCompactSerializationTest` that expects source present when `include_inherited=true` is passed there — both its tests pass `include_inherited=true`, so they now expect source PRESENT on all entries).
3. GREEN: `bool bIncludeInherited = false;` default; emit `source` on all entries when `bIncludeInherited && BP->ParentClass` produced inherited entries — emission keyed on the parameter (deterministic): compact emits `source` iff `bIncludeInherited` is true; verbose always.

## Task 6: compact pin default omission (C++)

Files: `Source/CortexGraph/Private/Operations/CortexGraphNodeOps.cpp` (SerializePin); test `CortexGraphCompactSerializationTest.cpp`.

1. RED: extend the compact get_subgraph pin assertions — compact pins have no `category` key (type already carries it), no `subcategory`/`sub_category_object` when empty, no `container_type` when `none`, no `is_reference`/`is_const`/`is_advanced`/`is_hidden` when false; verbose pins keep all of them.
2. GREEN: guard each field with `if (!bCompact || <non-default>)`. `category` is dropped in compact unconditionally (identical to `type`).

## Task 7: level.get_bounds scan_truncated (C++)

Files: `Source/CortexLevel/Private/Operations/CortexLevelQueryOps.cpp`; test `CortexLevelQueryTest.cpp`.

1. RED: `get_bounds` response has `scan_truncated` false on a normal level; `CollectFilteredActors` with `MaxScanCount=2` on a level with more matching actors sets the out flag true and the command path emits `scan_truncated: true`.
2. GREEN: `CollectFilteredActors(World, Params, OutError, MaxScanCount = 10000, *OutbScanTruncated = nullptr)`; `GetBounds` always emits `scan_truncated`.

## Task 8: verification

1. Build: UBT `CortexSandboxEditor Win64 Development`.
2. UE suites: `Cortex.Blueprint+`, `Cortex.Graph+`, `Cortex.Level+` (RunTests.ps1), zero unexpected warnings.
3. Python: staged `not e2e/scenario/stress` suite, then `e2e` if an editor is running.
4. Offline listing before/after via the measurement script (36/48,585 -> expected ~30 tools, ~-9,400 chars).
5. Clang syntax gate with no concurrent build.

## Task 9: sync, docs, release

1. Toolkit: regenerate `tests/fixtures/cortex-mcp-surface.json` (`CORTEX_PLUGIN_REF=<plugin sha>`), migrate `cortex-bp-migrate` references to the six wrappers to `reflect_cmd` forms, run the drift check, bump toolkit patch version.
2. Plugin docs: this design + impl doc, verification report `docs/verification/`, `docs/systems/` updates for the reflect surface and response policies.
3. Parent repo: request file `docs/requests/core/2026-11-18-core-issue225-token-cost-request.md` + INDEX row.
4. Plugin version bump 0.4.16 -> 0.4.17 (`UnrealCortex.uplugin`, `README.md`, `MCP/pyproject.toml`, `uv lock`), own commit.
5. LF-normalize every touched file before committing (CRLF worktree); push branch `fix/225-token-cost`, open PR referencing the issue, request review. No merge without explicit approval.
