# Issue 225 design: token cost of default reads and tool descriptions

Issue: https://github.com/etelyatn/UnrealCortex/issues/225.
Baseline: plugin `f305ab82` (0.4.16). Offline tool-listing measurement (fallback capabilities, no editor): 36 tools, 48,585 characters (descriptions 37,010 + input schemas 11,575). Composites are 21,955 of that; `blueprint_compose` alone is 7,711.

## Already resolved at baseline (no work)

- Schema `title` annotations: #212 (merged) strips them (12,196 -> 11,575 includes two new tools).
- `blueprint.list`, `data.list_gameplay_tags`, `level.find_actors`, `graph.search_nodes` and the other `CACHED_PAGE_ARRAYS` reads: Python cursor pagination (`routers.py`).
- `level.list_actors`, `editor.get_recent_logs`, `reflect.get_dependencies`/`get_referencers`/`find_usages`/`find_overrides`: native-owned windows (`NATIVE_LIMIT_COMMANDS`, native defaults 100/100/20/20/20).
- `graph.get_subgraph`, `umg.get_tree`, material/class-default reads: complete-or-refuse selector policies in `response.py` that never advertise an unconsumed `limit`.
- `umg.get_tree` unlimited depth default and `editor.get_recent_logs` default limit 200: reviewed #180 contracts (`2026-10-13-token-efficiency-design.md` §3.4, §4.1); unchanged.
- `data.list_gameplay_tags` O(N^2) child scan: server CPU only, rows are short and paginated; out of token scope.

## Decisions

1. **Composite descriptions** (`blueprint_compose` 7,711; `material_compose` 4,315; `widget_compose` 3,121): rewrite to one consistent description per tool.
   - `blueprint_compose`: the current text concatenates `_SAFE_UPDATE_DOC` with the legacy create-only docstring, which claims only `create` is supported while the outer text documents `mode='update'`. New text documents both modes coherently, keeps the create-mode spec reference (node classes, pin conventions) and the safe-update protocol, condensed. Stale tool names removed (`create_blueprint`, `add_blueprint_variable`, `graph_add_node`, `create_widget_screen`, example calling `create_blueprint_graph`); examples renamed to `blueprint_compose` and shortened.
   - `material_compose`/`material_instance_compose`: drop `create_material + add_material_node + connect_material_nodes` and the `create_material_graph(` example; examples renamed to the compose names.
   - `widget_compose`: drop `create_blueprint + add_widget + set_text + set_color` and the `create_widget_screen(` example.
   - `routers.py` `_schema_status` suggestion says `generate_project_schema`; the registered tool is `schema_generate`.
   - A registration test asserts no advertised description names a tool that is not registered (the stale-name regression class).

2. **Six duplicate reflect wrappers deleted** (`get_dependencies`, `get_referencers`, `query_class_detail`, `query_class_hierarchy`, `query_overrides`, `query_usages`): they wrap commands `reflect_cmd` already lists, at 5,687 characters of default listing. `reflect_cmd` forwards the same params (native windows are bounded; `class_detail`/`class_hierarchy` accept `detail`/`include_flat_classes`), so the capability is fully preserved through the router. Clean cutover per repo rules: modules, registration, `_BLUEPRINT_TOOLSET` membership (the five non-duplicate standalones remain) and tests are removed, not disabled; no compatibility aliases. The five non-duplicates stay: they wrap commands the router does not list (`scan_project`, `reflect_cache_status`, `rebuild_graph_cache`, `query_class_context`, `impact_analysis`) or add projection the router cannot.
   - Supersedes #180 design §2.2's "all 11 Reflect standalone tools" for the blueprint preset.
   - Toolkit: surface fixture regeneration, `cortex-bp-migrate` skill references migrate to `reflect_cmd` forms (some already are).

3. **`blueprint.get_info`**: `include_inherited` defaults to **false** (was true; every inherited callable of the parent was emitted by default). `source` is emitted on every function entry whenever inherited functions are listed (compact included), so own and inherited entries are distinguishable exactly when both can appear; verbose mode keeps emitting `source` unconditionally, compact without inherited keeps omitting it (uniform listing).

4. **Compact pin serialization** (`FCortexGraphNodeOps::SerializePin`): compact mode already omits `is_connected` when false and `default_value` when empty; extend the same default-omission to `category` (identical to `type`), `subcategory`/`sub_category_object` when empty, `container_type` when `none`, and `is_reference`/`is_const`/`is_advanced`/`is_hidden` when false. Detailed mode is unchanged.

5. **`level.get_bounds`**: the actor scan stops at 10,000 with no signal; the response now always carries `scan_truncated` (false normally, true when the cap cut the scan). `CollectFilteredActors` takes the cap as a defaulted parameter so the flag logic is testable without 10,001 actors.

6. **Truncation hints**: the generic array truncator in `response.py` always suggests `limit`; for reads that do not page, that parameter is refused natively and the model burns a repeat call. `format_response` gains `pages: bool = False`; the suggestion is the pagination text only when the caller pages, otherwise "this read does not page; narrow the request and re-run". Router fallback paths for `CACHED_PAGE_ARRAYS` commands pass `pages=True`. `query_class_hierarchy`-style flat reads and traces get complete-or-refuse policy entries naming their real selectors (`max_results`; `max_depth`).

7. **`CACHED_PAGE_ARRAYS` called without `limit`**: an oversized response is snapshotted into the pagination cache and page 1 (largest page that fits) is returned with `next_cursor`, instead of a binary-search truncation with a `limit` hint — the pattern `core.batch_query` already implements (`routers.py` `_batch_query`). Responses that fit are still returned whole.

## Non-goals

- No changes to reviewed #180 defaults (`umg.get_tree` depth, log limit 200).
- No gameplay-tag row slimming or O(N^2) rewrite (CPU; rows short and paginated).
- No new pagination for mixed nodes/edges reads (`get_subgraph`, traces): complete-or-refuse with named selectors stays their contract.
- No native response-policy framework; each change extends its owning serializer/router.

## Acceptance

| Criterion | Proof |
|---|---|
| Descriptions consistent, no stale names | Registration test: no description names an unregistered tool; `blueprint_compose` text documents both modes; offline listing shrinks |
| Six wrappers gone | tools/list has 30 tools; presets list five reflect standalones; python suites green |
| `include_inherited` default false + `source` | `Cortex.Blueprint+` tests: default omits inherited; `include_inherited=true` compact emits `source` on every entry |
| Compact pin omission | `Cortex.Graph+` compact test: default-valued pin fields absent, verbose unchanged |
| `scan_truncated` | `Cortex.Level+` tests: flag false normally, true when the (parameterized) cap cuts |
| Truthful hints | Python tests: non-paging overflow names no `limit`; trace/hierarchy policies |
| Page 1 + cursor | Python test: oversized no-limit `CACHED_PAGE_ARRAYS` read returns page 1 with `next_cursor` that continues correctly |

## Simplicity check

- [x] Think before acting — every issue row mapped against 0.4.16 source; resolved rows identified with their shipped mechanism.
- [x] Simplicity first — deletion over exclusion mechanisms; existing truncator/policy/pagination owners extended; no new frameworks.
- [x] Surgical changes — O(N^2) scan, reviewed #180 defaults and tag row weight excluded with reasons.
- [x] Goal-driven execution — acceptance table above; finite verification in the impl plan.
