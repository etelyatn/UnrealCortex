# Cross-domain token-efficiency design

Date: 2026-10-13. Status: independently reviewed; ready for implementation planning; not implemented.
Issue: https://github.com/etelyatn/UnrealCortex/issues/180.
Evidence: [cross-domain audit](2026-10-13-token-efficiency-audit.md).

## 1. Outcome, authority and boundaries

Reduce unnecessary model-facing context across the shipped MCP surface, not only the originally reported Blueprint styles. Preserve effective values when requested, exact identities, correct targeting, complete approval/readback, actionable errors and truthful mutation/persistence outcomes. Investigate every domain/mechanism, apply justified repeated patterns, and retain already-sufficient reductions.

This invocation authorizes local investigation, design/review and implementation-plan/review artifacts. It does not authorize product implementation, dependency installation, commits, push/PR/merge/closure, parent gitlink updates or changes to user snapshots. The user explicitly selected design subagent review followed by implementation planning and plan review; no extra routine workflow menu is needed. Product execution still requires approval of the resulting artifacts and execution authority.

Bases: plugin `9f899fafeb18666cc8204f7446120d411491861f`; toolkit `fb5f959c3e42a14296c802b904b355f99785d174`. UE supported floor remains 5.6; native runtime currently available for future validation is UE 5.8 on Windows. Python remains >=3.10. Implementation adopts `mcp>=1.26.0` (already locked/installed and capability-probed) so public structured_output=False and explicit CallToolResult semantics are supported; retain portable stdio.

Clean cutover: migrate every affected registered consumer, native schema, generated fallback, test and toolkit instruction; no compatibility aliases. No game types, cross-domain module coupling or new standalone MCP tools. UObject access and mutation remain Game Thread-only; save remains explicit. Do not replace existing graphs' approval/rollback/selector completeness contracts with generic projections.

## 2. Reuse-first architecture

Four independently reviewable workstreams share this contract:

1. **Python boundary/discovery:** one narrow public registration/encoding adapter, names-only router descriptions, startup toolsets, explicit pagination ownership and response-size accounting.
2. **Native targeted reads:** existing domain commands acquire selectors/summary views where extraction currently expands everything. Reuse Core leaf resolution and policy-driven serialization; preserve domain naming conventions.
3. **Repeated read/echo mechanisms:** canonical logs and QA cursors, lean catalogs/component/job lists, reflection summaries, class-default deltas/echo.
4. **Offline/context entry points:** linked schema catalog and same-message frontend context deduplication; toolkit guidance follows these contracts.

Do not create a generic native response-policy framework. Native domains remain authoritative for semantic extraction and guarded outcomes. The Python boundary controls MCP presentation, not Unreal transactions, class defaults, normalization or persistence. Durable export/schema/report/assets stay canonical on disk; conversational results stay compact.

### 2.1 Public MCP result encoding (TE01, TE17)

`MCP/src/cortex_mcp/tool_output.py` owns a small `ToolOutputRegistrar` passed by server._register_explicit_tools to ALL public router, composite and standalone registrations, including Reflect/import queues and optional Gen. It wraps public callables and uses the public `FastMCP.tool(..., structured_output=False)` API. It must preserve sync/async dispatch and exact input signatures, including the strict router extra-field rejection model. It delegates the existing _tool_manager access used solely by strict-router input validation; it does not patch SDK output internals.

Output-only helper `compact_json(value)` in existing response.py uses `json.dumps(value, separators=(",", ":"), ensure_ascii=False)`. Every response-budget calculation, including dedicated graph prune/rewire/retirement and UMG formatter paths, uses that same representation. Do not change canonical hashing, cache-key encoding, durable files, rollback journals or protocol input serialization merely to remove spaces.

Startup setting `CORTEX_RESPONSE_MODE=text|structured`, default **text**:
- text: exactly one compact JSON TextContent; no duplicate structuredContent.result string or automatic outputSchema for a string.
- structured: the same compact text plus the actual decoded object under structuredContent, never JSON inside a string. This deliberately pays for both representations for clients that need structured data.
- Existing explicit CallToolResult/image content is preserved. Non-JSON text failures remain one opaque text block with their original meaning; do not turn a parsing failure into empty success or infer authoritative native error codes.
- Domain errors/outcome envelopes are preserved before encoding. The registration adapter is not a generic semantic truncator. Legacy internal callable string returns can remain internal; public registered outputs have one encoding owner.

`MAX_RESPONSE_CHARS=40000` continues to bound model-facing compact JSON text, NOT the entire MCP envelope or UTF-8 bytes. Structured mode has additional wire cost. Dedicated preview/pre-mutation gates include their entire approval payload and must remain complete-or-refuse. Optional telemetry uses MCP `_meta.cortex` rather than adding body fields after a preflight. With `CORTEX_RESPONSE_METRICS=1`, expose `json_chars` and `utf8_bytes` excluding telemetry itself, and aggregate tool/command sizes in the existing connection metrics. No default approximate-token field, body logging, hidden retries or model-specific savings promise.

### 2.2 Descriptions and startup toolsets (TE02)

Router descriptions retain tool/domain routing, complete valid command names, canonical `{command, params}` envelope, on-demand `core.get_operation_schema` guidance and necessary authoring/trust/composite warnings. Remove every operation's parameter signature from both cache and fallback descriptions. Do not remove underlying signature metadata from the full capabilities cache or schema service.

Shorten composite/Reflect prose duplicated by their typed top-level schema or common guidance. Retain self-sufficient declarative structure examples, create-versus-update policy, expected fingerprints, explicit save/compile controls, unknown-outcome guidance and schema discovery. Native operation schemas do NOT describe Python composite specs; do not pretend they do or require unavailable resources for basic invocation. Fill missing short outcome descriptions on public wrappers currently publishing no description.

`CORTEX_TOOLSET=all|ui|blueprint|data`, default **all**, is a startup registration filter, not an authorization boundary:
- all: current registered surface; optional Gen/Animation still require capability availability.
- ui: core/data/blueprint/graph/umg/editor routers; blueprint_compose, widget_compose; profile_operation_schema, schema_generate, editor_restart.
- blueprint: core/data/blueprint/graph/editor/reflect routers; blueprint_compose; all 11 Reflect standalone tools; profile_operation_schema, schema_generate, editor_restart.
- data: core/data/editor routers; apply_import_ops_json, schema_generate, editor_restart.
Unknown settings refuse server startup with valid choices. A client/server process restart is required to change toolsets/descriptions. A visibility filter does not restrict native commands reachable through other permitted tools. Existing UMGAuthoring policy/profile retry-budget semantics remain separate and unchanged; use core.get_operation_schema for general-domain live contracts.

### 2.3 Pagination ownership (TE03)

Before generic limit validation, identify native-owned commands from the audit's explicit set and forward native limit/offset unchanged. Native-specific zero/caps/detail windows retain their semantics. UMG detail/binding/authoring and graph guarded branches retain priority. Do not reinterpret native limit=500 as a Python limit=200 or Gen limit=0 as invalid.

For cache-owned unbounded reads, replace largest-list guessing with a reviewed command-to-array mapping. Honor a requested page even when fewer than 10 items exist; the formatter's auto-truncation threshold is not an explicit-pagination rule. Cache pages preserve original count semantics separately from returned_count, snapshot identity and total; next_cursor is computed from the ACTUALLY returned page after size bounding, so subsequent pages cannot skip oversized omitted rows. A zero-row page cannot report has_more with a non-progressing cursor; refuse with actionable scope guidance instead.

Only simple unbounded read arrays enter that mapping. No schema objects, hierarchical trees, mixed catalogs, mutation results or approvable GUID sets. Unsupported cached cursor requests refuse before native dispatch. Native paging returns native totals/completeness and no Python cursor pretending the native window is the whole population. Existing TTL/LRU remain; no durable cursor store or implicit retry.

Native-owned pages are complete-or-refuse at the MCP size boundary: never shorten their primary array while retaining a native continuation computed for a larger page. An oversized native page returns an explicitly incomplete refusal, preserves the window/selector/count context, exposes no advancing continuation, and names a smaller limit/field selection. Even a single oversized record refuses honestly without skipping it. Cached pages may be shortened only with their continuation rebuilt from the emitted rows.

## 3. Native read contracts

### 3.1 Data discovery/filter consistency (TE04)

- data.search_assets uses `class_names:string[]` and `path_prefixes:string[]`: OR within each list, AND across class/path/query categories. Query is case-insensitive name-or-path text. Class matching retains exact class-name semantics; do not silently add subclass searches. Prefixes are canonical mounted package folders and recursive component-boundary matches, not arbitrary string prefixes. Malformed filter arrays refuse, rather than becoming unfiltered project scans.
- data.list_data_assets uses the advertised `class_name` and existing subclass-aware behavior. Migrate old class_filter callers; remove the obsolete name for this command only (unrelated raw-export contracts remain outside this cutover).
- search `fields` defaults `[path,name,class_name]`; allowed fields are those three and package_path. Explicit `[path]` is the lean identity view. Do not shorten canonical paths in-place. `count_only=false` default; true emits no rows, exact filtered matched count and readiness/completeness. Returned count and total_matches have different meanings. Add offset/default limit50 with returned/next_offset/is_complete metadata for normal pages, sorted deterministically by canonical object path before paging.

### 3.2 Class-default discovery versus effective-value reads (TE05, TE07)

Existing defaults reads are the reuse owner, not new list tools:
- get_class_defaults with nonempty properties defaults include_values=true and returns those top-level/leaf effective values; omission/empty properties defaults to **metadata discovery**, include_values=false.
- list_settable_defaults and list_inherited_properties default include_values=false. A properties string-array selects top-level/leaf fields before any value expansion; discovery still enforces root property admission and settable/inherited scope.
- include_values=true explicitly recovers values in all discovery commands; include_members=false default removes duplicate struct values. include_members=true adds the existing one-level member path/type metadata and values only when values are requested. Preserve FText/string-table metadata.
- Metadata discovery uses `discovery_offset=0`, `discovery_limit=100` (1..200) and reports total_properties, returned count and discovery_pagination `{offset,limit,returned,next_offset,is_complete}`. These names avoid generic cached limit interception. Selected explicit property reads are not silently paged by discovery offsets.
- In discovery mode, missing value is explicitly marked by `values_included=false`; callers must not treat it as null or an empty struct. Every selected leaf value, including zero/false/null, is retained in effective-value mode. Class/path/type/accepted_formats remain where relevant. No broad field-key shortening.
- Overflow is an incomplete/refusal envelope with the applicable properties/discovery_limit/include_values guidance and a bounded property-name index. Index size is capped and annotated; never fabricate exact per-property byte counts without serialization. Metadata discovery prevents value work by construction; exact full-data budget is still checked after actual serialization.

### 3.3 Selected object/node properties and safe deltas (TE05, TE06)

- data.get_data_asset gains optional fields (absent/empty = full current effective read); material.get_node gains optional properties (absent/empty = full current read). Use Core ResolvePropertyPath for dotted struct/non-null object paths, preserve root admission, refuse unknown paths. Arrays/sets/maps are selected atomically; no new element-addressing grammar.
- DataAsset selected values use PropertyToJsonDeep with the existing policy/partial/issues/localization behavior; do NOT feed a top-level name filter indiscriminately into recursive PropertyAdmissionRule. Issue paths correspond to selected paths, not omitted fields. Material node identity and collection/parameter selectors remain.
- Blueprint get_class_defaults gains changed_only=false. When true, include_values must be true, and response declares `value_mode=inherited_delta`, `baseline_class`, `effective_values_complete=false`. Only inherited properties with owner/type-compatible parent fields compare to parent CDO values. Subclass-declared properties have no inherited baseline and are always emitted. False/zero/null differing from a nonempty parent are explicit overrides. Selected equal-to-baseline properties are listed under omitted_equal_properties, not interpreted as missing fields.
- Reuse a narrowly exposed Core property-comparison serializer only after validating pointers/ownership in Blueprint; existing helpers are private and the whole-object helper's class matching is wrong for parent CDOs. Delta recursion retains type/localization and structured truncation if depth limits apply; do not expose the historical `<changed>` sentinel as a complete effective value. No implicit mutation/reset behavior.

### 3.4 Lean inventories, reflection and trees (TE08-11, TE13)

- data.get_data_catalog: optional sections from datatables/gameplay_tags/data_assets/string_tables/curve_tables, all by default; include_asset_paths=false by default. Class/count/example_path remains; full path list via include_asset_paths=true or list_data_assets(class_name). Both the data router and existing core facade forward selections; cache key includes them.
- Agent-facing core_cmd(get_capabilities) defaults detail=names, explicit detail=full restores the complete signature inventory. This is Python-owned presentation. UEConnection's native handshake and saved capabilities cache always use full data; cache is never overwritten by a projected agent read. Full native schema authority remains live, not cache-inferred.
- level.list_components: include_properties=false default; true retains the established up-to-20 editable property detail with total/returned property counts and properties_complete. Full leaves remain available through get_component_property. Do not silently expand components or omit identity/root/creation_method. Keep lightweight get_actor component-name summaries unchanged.
- query_class_detail defaults detail=summary; summary is forwarded to native before property/function extraction. Existing properties view remains supported via full native extraction then scoped Python projection (no new native mode required); invalid detail values refuse. query_class_context retains properties default but fetches parent native summary and bounds children honestly. Native reflect.class_hierarchy defaults include_flat_classes=false; explicit true recovers flat classes. Native reflect.search adds truthful returned/matched/truncated metadata; no returned length labeled as total matches. Maintain not_scanned and dependency coverage.
- umg.get_tree retains full depth by default, names_only=false, optional depth (root=0, integer>=0), optional max_nodes (integer>=1). Bound traversal BEFORE building child JSON. Omit children only for actual leaves; depth/node cuts emit child_count and incomplete markers, not a false leaf. Names-only keeps hierarchy/name/child counts; full keeps class and user-widget blueprint identity. Emit returned_widgets, total_widgets, tree_complete and reasons. Count-only traversal for total does not expand properties. include_property_bindings=true remains a separate complete-or-explicit-incomplete asset-wide inspection, unaffected by a tree cut; unsafe combined reads refuse rather than returning partial orphan state as empty.
- gen.list_jobs defaults include_prompt=false; include_prompt=true or job_status preserves full prompt. Job id/provider/status/progress/time/count remain. Native limit=0 means unlimited, not a cached pagination request; default limited-page policy remains the existing native default.

## 4. Repeated logs and mutation echoes

### 4.1 Canonical log stream and QA consumers (TE12)

Emit **entries**, not duplicate logs; each record retains cursor/timestamp/category/message/severity. Input is since_cursor, not cursor. QA scenario/test-step/detector consumers and BOTH copied detector modules migrate together; no legacy field fallbacks. Native limit defaults200 (1..2000); count_only=false, optional until_cursor is an inclusive capture watermark. Return matched_count, returned count, next_cursor, latest_cursor, window_end_cursor, has_more, oldest_available_cursor and lost_before_cursor. Remove the old ambiguous response cursor field in this cutover. count_only=true returns metadata without record strings, providing a cheap pre-action watermark.

First page without until_cursor captures the current latest cursor as window_end_cursor. Continuations pass that same until_cursor and apply time filtering relative to the retained end-of-window timestamp, not a moving new capture timestamp. New logs beyond that watermark belong to the next step. Ring eviction reports lost_before_cursor; if the entire requested window has been evicted, coverage is explicitly incomplete, never empty-and-complete. A bounded page cannot advance past omitted matching entries; scanned nonmatching entries may advance continuation. Empty retained windows use a non-regressing cursor.

The native `since_seconds=-1` sentinel disables timestamp filtering; nonnegative durations retain the captured-window time policy. QA uses -1 on baseline, first page and every continuation so an early retained error from an action/wait longer than30 seconds is not filtered away while claiming complete cursor coverage. Reject other negative durations rather than silently changing selection.

QA takes a metadata-only latest watermark before each action, then consumes the finite retained end-of-step window through all pages before detecting/finalizing that step. This is bounded pagination, not polling for future logs. The same fixed watermark is used on every continuation; findings beyond page one of the final/only step remain attributable to that step. test_step's monitored scope is action/wait, matching its existing detection location; assert/screenshot outcomes retain their separate statuses. Return log_coverage with complete, window_start_cursor, window_end_cursor, pages_read and loss/refusal detail. Ring loss, oversized-record refusal or unavailable reads make log_coverage.complete=false without falsely downgrading the already-applied action outcome; never claim fully checked coverage. Scenario reports retain this per-step state. Severity detection reads severity and detects error text without an 'Error:' prefix. Remove unsupported max_lines. Capture-ring capacity/lifetime remains unchanged; no new log store or implicit retries.

The screenshot filename/output_path discrepancy discovered during audit is unrelated to token usage and is explicitly not in this implementation scope.

### 4.2 Class-default echo contract (TE14)

set_class_defaults gains echo=diff|full|none, default diff; the setting applies per batch item under the existing item contract. Diff records carry type, success/changed and each changed scalar leaf's previous/new effective value. Structs recurse; arrays/maps/sets are atomic values, additions/removals/nulls explicit. Full preserves previous/new representation; none omits value echoes only. All modes retain target identity, written/unwritten outcomes, guards, errors/warnings, compile/save/persistence and recovery state. The journal/internal previous values used for rollback are not the echo and are never dropped. No global pruning of other domains' canonical write selectors or authoring response envelopes.

Echo comparison cannot rely on legacy JSON alone for 64-bit integers: PropertyToJson currently casts int64 to double. The class-default owner captures an exact scoped native snapshot for precision-sensitive property roots before assignment and uses native typed comparison/rollback for those roots. Snapshot lifetime ends before explicit compile/reinstance; retained UObject references and property alignment/destruction remain valid. This is local to class-default writes, not a global serializer/setter change. Unsafe-to-double integer echo leaves use exact decimal strings with explicit value_encoding metadata, following the existing StateTree inspection pattern; do not claim an unchanged value from two rounded JSON numbers. Ordinary precise JSON paths keep their existing representation. Echo reports actual normalized post-state, not a promise that the legacy setter accepts arbitrary exact wide-integer input.

## 5. Offline context and frontend (TE15-16)

_catalog.md is an entry point, not a second class catalog: counts plus a relative link to blueprints.md, data indexes, freshness/version and project overview. blueprints.md remains complete/deterministic; no changes to durable class identity or existing snapshots during planning.

Frontend gathers editorContext once per send and reuses that snapshot for an explicit provider chip and auto-context. If the explicit chip successfully supplies that context, do not prepend it a second time. Keep user message, chip labels/other chip types, opt-in defaults and empty/failure fallback intact. No cache across messages: changed selection/world/editor state must appear next turn. Codex default instruction block emits access mode once; preserve reassertion on every resumed turn and system-vs-user precedence. Conversion/analysis safety prompts and provider lifecycle are unchanged. Check Claude/Codex stream display decoders accept text-only and object representations; migrate only observed incompatible parsing, not UI layout.

Toolkit changes are guidance-only: prefer targeted selectors/metadata discovery and summary views, use startup toolsets without confusing authoring policies, read linked schema/context on demand, and use canonical log/filter names. Keep all safety contracts. Do not invent prefetch generation or rewrite unrelated skills.

## 6. Alternatives and exclusions

Chosen: existing-owner selectors/summary contracts plus one Python public boundary. Rejected: global shortest-key/TSV formats, eager schema dumps, new inspection tools for every projection, a native response-policy framework, automatic cross-turn content caching, domain-dependent graph serialization, and silent zero/default omission.

Retained adequate mechanisms and incidental findings are exhaustively recorded in the audit. No migration of archival schema/reference formats, arbitrary batch data elision, graph.describe_node contract slimming, screenshot binary inlining, provider permission changes, asset-chip implementation or speculative retry/telemetry infrastructure. Full-mode graph aliases and other tiny duplicate counters/status keys remain because defaults are already compact or the field is useful; no unbounded cosmetic audit is a delivery gate.

## 7. Finite acceptance and release gates

| Criterion | Minimal proof and exit condition |
|---|---|
| A1 Encoding/telemetry | Actual stdio list/call on registered router, composite and standalone in both modes; same semantic data, single text, no nested JSON-string wrapper; Unicode/null/numbers preserved; invalid/opaque text and async result preserved. Dedicated graph/UMG complete/refusal/applied outcomes pass unchanged semantics under compact budget. Metrics off adds no metadata; metrics on reports len(compact_text) and len(compact_text.encode('utf-8')) excluding its own metadata, with Unicode differing correctly, exact per-tool/command aggregates and unchanged outcome/approval budgets. |
| A2 Discovery/toolsets | Actual tools/list for all/ui/blueprint/data, cached and fallback description paths; exact exposed names and optional-domain availability; strict invocation and on-demand native schemas still usable; composite spec examples sufficient for one real public call. |
| A3 Paging | Real/native-backed fixtures exceed native defaults; honor requested limits/offsets; cache read with <10 entries and oversized rows has contiguous, non-skipping forward progress; unsupported cursor and guarded mutation pagination refuse. |
| A4 Search/read selection | Disposable assets inside/outside canonical folder plus class filters; exact counts/pages/count_only; DataAsset/material/class leaf reads exclude unrelated heavy values, retain zero/false/null/FText and partial issues; invalid selectors do not fall through to full reads. |
| A5 Discovery/default delta/echo | Metadata-only discovery performs no value extraction and has honest page totals; explicit full and members recover detail. Parent overrides/subclass values are safe; delta is labeled and omission reported. Real echo modes have identical applied/persisted state and failure outcome. Precision boundary native before9007199254740993 -> normalized after9007199254740992 reports changed with exact decimal echo, and a later-property failure restores the exact native old integer in every echo mode. Struct/container and GC-before-compile snapshot lifetimes are proved without global serializer/setter changes. |
| A6 Inventories/tree/reflection/jobs | Large catalog/component/tree/reflection/job fixtures show smaller representative model-facing bytes with full retrieval available; real tree cuts never look like leaves and asset-owned binding state is preserved/refused honestly; native summary skips extraction. |
| A7 Logs/scenarios | Native records >page cap, filtered gaps, new records beyond a fixed watermark, empty windows and ring-loss boundary; no duplicate array, skipped matching record or repeated step finding. The only error is beyond page one of the final/only QA step and is attributed once to that step. A retained early error more than30 seconds before the window end is detected by cursor-only QA reads. Ring-loss/oversize refusal reports incomplete coverage; error severity without textual prefix is detected. |
| A8 Schema/context | Generated catalog links resolve to complete class table; freshness unchanged. Rendering-editor send captures one same-turn context snapshot, next turn reflects changed state; access policy remains in default/custom/resumed Codex prompts. Display result decoding handles both result modes. |
| A9 Required integration gates | Supported build, serial affected/full native suites with zero unexpected warnings, staged Python suites, actual public-route CortexSandbox benchmark, toolkit synchronization/docs review. Bind exact plugin/toolkit candidate/base/source identities. No editor/runtime = integration blocked, not green. |

Compare representative before/after model-facing UTF-8 bytes and tools/list characters; report actual tokenizer counts only if a specified tokenizer is used. The issue's 60-80% estimate is not a required fabricated number. Stop when these criteria and mapped review findings pass; new checks require an unmet criterion, actual failure or source-confirmed reachable violation. Docs-only revisions bind ancestry/source identity without rerunning unchanged suites. Publication remains separately authorized.

## Simplicity check

- [x] Think before acting — all domains/mechanisms covered by the linked source audit; protocol/SDK output measured; native runtime limit explicit.
- [x] Simplicity first — existing read owners, one registration boundary, existing live schemas and durable exports; no generic native framework.
- [x] Surgical changes — planned selectors/context contracts only; incidental unrelated defects and user snapshots excluded.
- [x] Goal-driven execution — finite A1-A9 evidence, preserved safety and explicit non-implementation status; independent review precedes planning.
