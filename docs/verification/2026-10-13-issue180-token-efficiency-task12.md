# Issue 180 — Token efficiency, Task 12: native summary before extraction and honest hierarchy/search

**Date:** 2026-10-13 (native runs recorded 2026-10-14)
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `7b62ef4` (plugin HEAD at staging; Tasks 1/2/3/4/9/10/15 committed above it)
**Owner:** Task 12 implementer (`ImplTask12Reflect`) — the RED regressions were staged by the prior
implementer (`Task12ReflectLean`), the production GREEN implementation and this record by
`ImplTask12Reflect`; all builds, test runs and the registered live smoke are controller-owned.

## Scope

Reflect detail reads ask the native side for the view that is actually needed instead of always
paying for a full extraction, the class hierarchy stops duplicating its whole tree as a second flat
array by default, and class search reports honest page/match/truncation metadata instead of labelling
a returned page length as a total.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexReflect/Private/Operations/CortexReflectOps.cpp` | `ClassDetail` (~cpp:1119-1130) validates the supplied `detail` and returns `InvalidField` for anything other than `summary`/`full`. `ClassHierarchy` (~cpp:651-654) reads `include_flat_classes` (default `false`) and (~cpp:744-766) builds/emits the flat `classes` array only when true; the reflect cache params (~cpp:773) record the opt-in. `BuildHierarchyTree` (~cpp:544-560) threads a `bool& bOutTruncated` out-param set whenever the `MaxResults` node budget stops the walk early, and `ClassHierarchy` emits it as the authoritative `truncated` boolean (~cpp:736-738). `Search` (~cpp:2136-2230) counts every match before applying the page, materializes at most `limit` rows, and emits `returned`/`total_matches`/`truncated`; the legacy `total_results` field is deleted outright. |
| `Source/CortexReflect/Private/Operations/CortexReflectOps.h` | `BuildHierarchyTree` declaration gains the `bool& bOutTruncated` parameter. |
| `Source/CortexReflect/Private/CortexReflectCommandHandler.cpp` | Live command info: `class_hierarchy` advertises the optional `include_flat_classes` boolean; `class_detail`'s `detail` description states summary/full and that any other value is refused. |
| `MCP/src/tools/reflect/detail.py` | `query_class_detail` default becomes `detail="summary"`; an invalid value refuses before any native dispatch; the native level is forwarded (`summary`/`full`; `properties` → `full`); new `_COVERAGE_FIELDS` (`cdo_unavailable`, `_message`, `error`, `error_code`) survive every projection. |
| `MCP/src/tools/reflect/context.py` | `query_class_context` validates `detail`, forwards the requested level for `self`, fetches the parent with native `detail="summary"`, and derives the single `children_complete` field **solely** from the native `truncated` traversal signal (the returned-length heuristic is deleted). |
| `Source/CortexReflect/Private/Tests/CortexReflectHierarchyTest.cpp` | Staged RED `Cortex.Reflect.Hierarchy.FlatClassesDefaultOff` / `FlatClassesOptIn`, plus the correction-round regression `Cortex.Reflect.Hierarchy.BoundedTraversalDeclaresTruncation` (budget-bounded walk with emitted children below `max_results` must declare `truncated=true`). |
| `Source/CortexReflect/Private/Tests/CortexReflectSearchTest.cpp` | Staged RED (unchanged): `Search.Basic`/`Limit` migrated to the truthful metadata, new `Search.UnderLimitComplete`, and `ClassHierarchy.ProjectPluginBlueprintVisible` opts into `include_flat_classes=true`. |
| `MCP/tests/test_reflect_tools.py` | Staged RED wiring/child-bounds regressions, plus the correction-round `test_truncated_below_emitted_cap_declares_incompleteness`. |
| `docs/systems/cortex-reflect.md` | Parent-repo Reflect doc: summary default, invalid-detail refusal, `children_complete`, `include_flat_classes` opt-in and the `returned`/`total_matches`/`truncated` search metadata (former `total_results` removed). |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task12.md` | This record. |

Not part of this task (deliberately): the generated MCP fallback and capabilities fixtures, and the
toolkit Reflect guides — Task 17 owns those. `MCP/src/tools/reflect/hierarchy.py` is unchanged: its
collector (`_walk_tree`/`_flatten_hierarchy`) walks `children` only and never consumes the native flat
array, so the Python flat list still works with the native default off.

## Contract implemented

`reflect.class_detail` (native)

* `detail` accepts `summary` (case-insensitive) or `full` (the default when omitted). Any other
  supplied value — including a standalone-only value the native op does not implement, e.g.
  `properties` or `verbose` — refuses with `INVALID_FIELD` instead of silently performing the full
  (most expensive) extraction.
* The `summary` path remains identity/parent/routing: `name`, `type`, `parent`, `module`,
  `source_path`/`asset_path`, `interfaces`, `blueprint_children_count`, with no property/function/
  component extraction.
* `not_scanned`/dependency semantics live in the usages/impact readers and are untouched.

`query_class_detail` (MCP standalone)

* Default `detail="summary"`: the reader forwards `detail="summary"` to the native read, so identity/
  parent/routing is returned before any member extraction.
* `detail="properties"` and `detail="full"` both forward native `detail="full"` and then apply the
  established Python allowlist (native has no separate properties mode).
* Any other `detail` value returns `Error: invalid detail '<value>'; …` and issues **no** native
  request.
* Coverage/error metadata (`cdo_unavailable`, `_message`, `error`, `error_code`) is not a
  presentation level and survives every projection, so a caller can tell a genuinely empty result
  from an unreadable one.

`query_class_context` (MCP standalone)

* Keeps its `properties` default; the requested class (`self`) is fetched at the requested level,
  while the parent is fetched through the **native summary** read (full extraction for the parent
  would be wasted work).
* Bounds children and reports exactly one honest completeness field, `children_complete`, derived
  **solely** from the native `truncated` traversal signal. A bounded child page is never presented as
  a complete inventory, even when its emitted children are far below the requested 20-node cap;
  `children_count` always equals the returned child count.

`reflect.class_hierarchy` (native)

* `include_flat_classes` defaults to `false`: the nested `children` tree is the navigable
  representation and the flat duplicate `classes` array is not emitted.
* `include_flat_classes=true` recovers the unchanged flat list (every tree node name once).
* The opt-in is recorded in the reflect cache params so a cached tree is never mislabelled.
* `truncated` (boolean) is the authoritative traversal-exhaustion signal: true when the
  depth/`max_results` walk stopped early. It is produced by the walk itself and is not recomputable
  from the emitted children, so a bounded hierarchy is reported incomplete even when fewer children
  than the cap were emitted.

`reflect.search` (native)

* `returned` (rows actually emitted), `total_matches` (every matching class, counted before the
  `limit` page is applied) and `truncated` (`total_matches > returned`).
* The legacy `total_results` field is **removed** — no alias, shim or re-export. No returned length
  is labelled a total.

## Rulings applied (controller)

1. **`total_results` removed, not aliased** (clean cutover), replaced by the truthful
   `returned`/`total_matches`/`truncated`.
2. **One honest child-completeness field: `children_complete`** — derived solely from the native
   `truncated` traversal-exhaustion signal; a bounded page is never presented complete.
3. **Unknown `detail` refuses at both layers** — Python before dispatch, and native `InvalidField`
   (the native op does expose a `detail` parameter, verified in source).

## Controller-observed RED (before implementation)

| Target | Result | Log |
|---|---|---|
| `Cortex.Reflect+` | 62 tests: **58 PASS / 4 FAIL** — `Hierarchy.FlatClassesDefaultOff`, `Search.Basic`, `Search.Limit`, `Search.UnderLimitComplete` | `Saved/TestLogs/AutomationTest_2026-10-14_004019.log` |
| `MCP/tests/test_reflect_tools.py` | **6 FAIL** (default/explicit detail must forward `detail=summary`; invalid detail must refuse without native dispatch; projection must keep `cdo_unavailable`/`_message`; context parent via native summary; bounded children must declare incompleteness) | pytest output (controller-owned) |

RED was consumer-visible: the default standalone detail was `full`; the native request carried no
`detail`; an invalid `detail` silently degraded to a full extraction; `_FULL_FIELDS` dropped
`cdo_unavailable`/`_message`; the context parent was fetched without `detail`; the reader reported
`children_count` with no incompleteness marker; the hierarchy always emitted `classes`; and search
carried `total_results = len(results)` (a returned length labelled a total).

## Controller-observed GREEN (after implementation)

| Target | Result | Log |
|---|---|---|
| `Cortex.Reflect+` | 62 tests performed, **62/62 PASS** (0 Fail) | `Saved/TestLogs/AutomationTest_2026-10-14_010423.log` |
| `MCP/tests/test_reflect_tools.py` + `test_class_defaults.py` | **33 passed** — the six previously-failing reflect cases now pass | pytest output (controller-owned) |

Raw `Warning|Ensure|Fatal|Assertion` scan of the native GREEN run log: clean.

## Correction round: authoritative traversal-exhaustion signal

Review round (controller-source-confirmed P2): `context.py` derived `children_complete` partly from
`1 + len(children) < _CHILDREN_MAX_RESULTS`, but native `reflect.class_hierarchy` emitted no
truncation/completeness field (`truncated` existed only for `reflect.search`), so the "native signal"
branch was dead and the length heuristic alone decided. The emitted children can be far below the cap
while the traversal was bounded (filtered/skipped intermediaries and deeper nodes consume the same
`MaxResults` budget), so a truncated hierarchy could be reported complete.

Correction:

* Native `BuildHierarchyTree` (~cpp:544-560) threads a `bool& bOutTruncated` out-param, set whenever
  the `MaxResults` node budget stops the walk early (a candidate child remains unvisited), and the
  header declaration gains it; `ClassHierarchy` emits it as the response `truncated` boolean
  (~cpp:736-738). It reflects the traversal, never the emitted array, and is not recomputable from the
  returned children.
* `context.py` deletes the returned-length heuristic; `children_complete` is now
  `not bool(hierarchy_data.get("truncated", False))` — derived solely from the native signal.
* Regressions added: `Cortex.Reflect.Hierarchy.BoundedTraversalDeclaresTruncation` (native:
  `root=AActor`, `depth=2`, `include_engine=true`, `max_results=3` — the budget is spent on the root
  and descendants, the emitted child list is shorter than the budget, and the response must still
  declare `truncated=true`) and
  `TestContextNativeSummaryAndChildBounds.test_truncated_below_emitted_cap_declares_incompleteness`
  (Python: a native page with `truncated=true` and only 3 emitted children — far below the 20-node cap
  — must yield `children_complete=False`).
* Forwarding: the signal lives in the native response `data`; `send_command_cached`/`ResponseCache`
  store the whole response, and `context.py` reads `truncated` from `hierarchy_data` with no
  intervening projection, so no value is dropped.

Covering runs for the corrected source (native `Cortex.Reflect+` including the new regression, and the
Python `test_reflect_tools.py` with the new case) are controller-owned and are **not** claimed here.

## Cache identity

`send_command_cached` keys on the full params dict
(`ResponseCache.make_key` → `json.dumps(params, sort_keys=True)`, `MCP/src/cortex_mcp/cache.py`), so
the forwarded `detail` and `include_inherited` are part of the key; switching the detail view issues a
distinct native read rather than returning a stale full/default payload.

## Source-confirmed rulings and deliberate pins

* **Both-layer refusal.** The standalone tool refuses before dispatch, and the native op (which does
  expose `detail`) refuses an unknown level rather than ignoring it. The native refusal is
  source-enforced but not pinned by a native regression — the staged RED scoped the invalid-`detail`
  refusal to the standalone Python tool, and `CortexReflectClassDetailTest.cpp` is outside this task's
  file list.
* **No new native mode.** `properties` remains a Python projection over native full, so only
  `summary` requests the reduced native read; no reader forwards `properties` to native (verified:
  the only native `reflect.class_detail` senders are `detail.py` and `context.py`).
* **One honest completeness field.** `children_complete` is the single emitted field and mirrors the
  native `truncated` traversal-exhaustion signal (carried through the cached response data; no
  projection drops it). The returned-length heuristic was removed because filtered/skipped
  intermediaries and deeper nodes can consume the native budget while the emitted children stay below
  the cap.
* **Clean cutover.** `total_results` is deleted outright; the migrated native tests assert its
  absence and assert `total_matches` is distinct from `returned`.

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` (and the generated capabilities fixtures) still describe
  `reflect.class_hierarchy` without `include_flat_classes` and do not carry the new search metadata;
  regenerate after a live capabilities read. The native command info already advertises the new
  option.
* Toolkit Reflect guides still describe the previous detail/default/hierarchy/search behaviour; the
  parent-repo `docs/systems/cortex-reflect.md` is already updated in this record's diff.
* The registered live `query_class_detail`/`query_class_context`/`hierarchy`/`search` smoke against
  task-owned classes and the zero-warning scan of that smoke are controller-owned and are **not**
  claimed here.
