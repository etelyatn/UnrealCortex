# Issue 180 — Token efficiency, Task 10: honest native UMG tree bounds, names projection, independent binding inventory

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `6e7421c5d998c2a5746821418b04f192996808ed` (Task 4 completion; this is the plugin
`HEAD` at the time of the task, with the Task 1–16 work uncommitted in the working tree)
**Owner:** Task 10 implementer (`ImplTask10UmgTreeBounds`) — RED test staging, production
implementation, the task report and this record; all builds, test runs and the registered live smoke
are controller-owned.

## Scope

`umg.get_tree` keeps full depth by default, but now honours an explicit traversal budget and a
names-only projection, reports how complete the walk actually was, and refuses an unsafe combined
tree + property-binding read instead of emitting a complete-looking partial payload.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.h` | Private `FTreeReadBudget` (the one traversal budget: `MaxDepth`, `RemainingNodes`, `bNamesOnly` plus the accounting `ReturnedWidgets`/`bDepthCut`/`bNodeCut`) and the bounded `BuildWidgetTreeJson(UWidget*, int32 Depth, FTreeReadBudget&)` signature. |
| `Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp` | Strict optional param reads (`ReadBoundedTreeIntParam`/`ReadBoundedTreeBoolParam`, cpp:201-238); budget-inspected, names-projected builder (cpp:241-321); `GetTree` (cpp:444-537) parses the bounds, counts the asset total without property expansion, emits the completeness fields and applies the combined-read refusal, which measures the **compact** condensed-writer text of the built response. |
| `Source/CortexUMG/Private/CortexUMGCommandHandler.cpp` | The live `get_tree` command info (cpp:186-191) advertises the optional `depth`, `max_nodes` and `names_only`; the description names the bounded/names-only contract. |
| `Source/CortexUMG/Private/Tests/CortexUMGTreeBoundsTest.cpp` | Staged RED: six consumer-visible regressions `Cortex.UMG.TreeBounds.*`, plus `Cortex.UMG.TreeBounds.CompactAdmissibleCombinedReadAccepted` added with the review correction below (seven regressions total; untracked). |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task10.md` | This record. |

Not part of this task (deliberately): the generated MCP fallback and capabilities fixtures, the
toolkit resources and the parent-repo/toolkit UMG documents — Task 17 owns those.

The first implementation round (the three production/test files above plus this record) is committed
as `beb1cb8` ("Task10: honest bounded UMG tree reads with truthful cut metadata"). The review
correction round documented below is uncommitted on top of that commit and touches only
`Operations/CortexUMGWidgetTreeOps.cpp`, `Tests/CortexUMGTreeBoundsTest.cpp` and this record, so
`git status --porcelain -- Source/CortexUMG` currently lists exactly the corrected `.cpp` and the test
file.

## Contract implemented

`umg.get_tree`

* Params: `require asset_path`; optional `depth` (integer `>= 0`, root is `0`; omitted keeps full
  depth), optional `max_nodes` (integer `>= 1`, counts the root and every emitted node; omitted is
  unlimited), optional `names_only` (boolean, default `false`), optional `include_property_bindings`
  (boolean, unchanged).
* A present bound must be an actual JSON number/boolean, integral and in range; JSON strings,
  fractional numbers, negative `depth`, `max_nodes < 1`, non-finite values and `max_nodes` beyond
  `int32` refuse with `INVALID_FIELD` before any asset work. Booleans are strictly typed, so a numeric
  `names_only` refuses rather than coercing.
* Response: `root` (or `null` for an asset with no root widget), `total_widgets` (count-only traversal
  of the whole asset tree — it never expands widget properties, so it stays exact under a cut),
  `returned_widgets` (nodes actually emitted), `tree_complete`, and `reasons` (array; empty on a
  complete read, otherwise it names the applied bound that cut the walk).
* Node shape: `name` always; `class` plus the `is_user_widget`/`widget_blueprint` breadcrumb for
  nested user widgets unless `names_only=true`; `child_count` = the node's **real** child total
  whenever it has children (complete or cut); `incomplete=true` **only** on a node whose walk stopped
  early; `children` only when at least one child was actually retained.
* A cut branch therefore declares `child_count` + `incomplete` and omits `children` — it is never
  presented as a leaf, and a complete node never claims a cut. Actual leaves omit both `children` and
  `child_count`. The single depth/node budget is inspected before any child JSON is allocated.
* `include_property_bindings=true` remains an independent complete asset-wide inventory appended by
  its unchanged owner (`FCortexUMGPropertyBindingOps::AppendInspection`); a tree cut never shortens it,
  and orphan diagnostics still resolve against the full tree with case-sensitive lookup.
* The one exception: when the inventory is attached and the **compact** JSON text of the whole built
  response exceeds the module's established bounded-response ceiling
  `CortexUMGAnimationTrackUtils::MaxDetailedResponseChars` (40000 characters), the combined read
  refuses with the module's bounded envelope — `_error = "RESPONSE_TOO_LARGE"`, `reader_complete =
  false`, retained `asset_path` / `total_widgets` / `returned_widgets` / `max_response_chars`, and no
  `root`, `property_binding_state` or `tree_complete` — instead of returning partial orphan state as
  empty.
* The measured quantity is exactly the text this command ships to the model: the built response
  object is serialized with the transport's condensed writer
  (`TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>` through `FJsonSerializer::Serialize`) and
  the resulting `FString::Len()` is compared with the 40000 ceiling. It is **not** the pretty-indented
  estimate (`EstimateAsciiPrettyChars`, which pads every number to 32 characters and adds per-element
  indent) that the animation family measures, so a combined read whose compact payload fits the
  ceiling is never refused.
* A tree-only oversized read is deliberately not refused natively: the tree is caller-boundable via
  `depth`/`max_nodes` (and its completeness is declared by `tree_complete`), and bounded tree-only
  responses are owned by the Python layer's own `RESPONSE_TOO_LARGE` refusal.

## Controller-observed RED (before implementation)

Rendering runner, serial, one filter at a time. Log
`Saved/TestLogs/AutomationTest_2026-10-13_233735.log` ("...Automation Test Queue Empty 6 tests
performed."): `Cortex.UMG.TreeBounds+` — 6 tests, **6 FAIL, 0 PASS**, 50 `LogAutomationController:
Error:` lines (the 6 per-test `Result={Fail}` completions plus 44 assertion failures), and zero
`Warning|Ensure|Fatal|Assertion` lines in the raw scan.

Per-test consumer-visible failures (test file line numbers as reported by the runner):

* `DepthZeroCut` — `tree_complete`, `child_count`, `returned_widgets` and `reasons` were all absent
  (`returned_widgets` read back as `-1`); `depth=0` was ignored entirely.
* `MaxNodesCut` — `max_nodes=2` was ignored: the read declared completeness and retained **2** children
  instead of 1, with no `incomplete` and no `child_count` on root or on the cut branch.
* `NamesOnlyHierarchy` — completeness/returned fields absent (`-1`); `class` still present on root and
  nested panel; `child_count` absent; the leaf still carried an empty `children` array.
* `BindingOrphanUnderCut` — the read claimed completeness, so the "tree really is cut" guard failed
  (the binding-inventory assertions themselves were already green and guard the inventory against a
  future cut).
* `OversizedCombinedReadRefused` — a plain complete `root` + `total_widgets` + `property_binding_state`
  success came back with no size bound at all.
* `FullTreeRecovery` — `tree_complete`/`returned_widgets` absent (`-1`) and leaves still serialized an
  empty `children` array.

The `Cortex.UMG.TreeBounds` family is now seven regressions; the run above covered the six staged RED
ones, and the seventh, `Cortex.UMG.TreeBounds.CompactAdmissibleCombinedReadAccepted`, was added with
the review correction below and has no RED of its own: it pins the corrected compact-text ceiling
against a payload that a pretty-estimate ceiling would have refused, so it fails only if the metric
regresses.

## Controller-observed GREEN — first implementation round (pre-correction source)

The two runs below covered the first implementation (the pretty-estimate ceiling); they are the
observed evidence for that source and are superseded for the corrected source by the round recorded
next.

| Filter | Result | Log | Raw diagnostic scan |
|---|---|---|---|
| `Cortex.UMG.TreeBounds+` | 6 tests performed, **6/6 GREEN** (0 Fail) | `Saved/TestLogs/AutomationTest_2026-10-13_235436.log` | 0 `Warning|Ensure|Fatal|Assertion` lines |
| `Cortex.UMG.PropertyBinding+` | 13 tests performed, **13/13 GREEN** (0 Fail) | `Saved/TestLogs/AutomationTest_2026-10-13_235524.log` | 0 `Warning|Ensure|Fatal|Assertion` lines |

Both logs end with a queue-empty line for their own filter (`6 tests performed.` and
`13 tests performed.`), i.e. the runs covered exactly the intended families. The runs are
controller-owned; the numbers above are read from those logs.

## Correction round: compact-response ceiling (review finding fix)

Review finding (controller source-confirmed, P2): the combined-read refusal compared a
pretty-indented estimate with a ceiling whose constraint is the model-facing **compact** text, so it
over-refused combined reads whose shipped payload fits (for example a tree of a few hundred
short-named widgets).

Correction applied (UMG tree branch only):

* The refusal now serializes the built response with the repo-wide condensed writer
  (`TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>` + `FJsonSerializer::Serialize` →
  `FString::Len()`, the same pattern as `CortexCommandRouter.cpp:98-101` and
  `CortexTcpServer.cpp:768-770`) and compares that length with the same 40000-character ceiling. No new
  estimator family was added, and the animation/authoring pretty-ceiling reads are untouched.
* The `RESPONSE_TOO_LARGE` envelope is unchanged: `_error`, `reader_complete=false`, `asset_path`,
  `total_widgets`, `returned_widgets`, `max_response_chars`, with no `root` /
  `property_binding_state` / `tree_complete`.
* The genuinely oversized fixture (120 children × 512-character fill) still refuses — its compact text
  is ~67.8 kB — and the six existing regressions are unchanged.
* New boundary regression `Cortex.UMG.TreeBounds.CompactAdmissibleCombinedReadAccepted`: 450 children
  with a 25-character fill. Trees of that shape carry a pretty-indented estimate of ~47 kB (tree only)
  to ~48 kB (with the inventory) while the shipped condensed text is ~32.5 kB (tree only) to ~33.2 kB
  (with the inventory); the test asserts the read is accepted, `tree_complete=true`,
  `returned_widgets == total_widgets == 451`, `reader_complete=true`, the seeded record retained, and
  measures the returned payload with the same condensed writer to prove
  `30000 < compact < 40000` with no `_error`.

Covering run identity for the corrected source: **to be recorded here from the controller's serial
`Cortex.UMG.TreeBounds+` (7 tests) and `Cortex.UMG.PropertyBinding+` (13 tests) runs.** This record
does not claim that GREEN.

## Bindings proved unchanged

* `Source/CortexUMG/Private/Operations/CortexUMGPropertyBindingOps.{h,cpp}` has **zero** changes, and
  `GetTree` still calls the same `AppendInspection(WBP, Params, nullptr, Data, …)` before any tree
  work, so the tree budget cannot reach the inventory path.
* `Cortex.UMG.PropertyBinding+` — the full 13-test binding family, including
  `ExactWidgetNamePreservesCaseOnlyOrphans` and `Persistence.ExplicitLifecycle` — was 13/13 GREEN on
  the pre-correction source (`235524.log`). The binding owner has zero changes in either round, so the
  tree-branch correction cannot alter any of those tests; the corrected source's run identity is
  recorded in the correction round above.
* `Cortex.UMG.TreeBounds.BindingOrphanUnderCut` additionally pins that a genuinely cut read
  (`tree_complete=false`) still returns `reader_complete=true`, `total=2`, `scope="asset"`, both
  serialized records (the case-only orphan keeping its exact raw name) and exactly one exact
  `Missing target widget: rootSibling` diagnostic.

## Consumer audit for real leaf omission

No consumer indexes a UMG tree node's `children` directly, so no consumer needed an edit:

* `MCP/src/cortex_mcp/verification/umg.py:12,23` and `MCP/src/tools/umg/composites.py:47,57,108` read
  `node.get("children", [])`.
* `MCP/src/tools/umg/widget_tree.py:73-75` only formats the payload.
* Native consumers `Source/CortexUMG/Private/Tests/CortexUMGAddWidgetTest.cpp:80-84` (`TryGetArrayField`)
  and `CortexUMGFullWidgetBuildTest.cpp:99-102` (`total_widgets`) tolerate an absent array.
* A workspace-wide search for `["children"]` finds only the unrelated reflect tool's own output
  (`MCP/src/tools/reflect/context.py:72,75`), and `MCP/tests/test_e2e.py::TestUMGCRUD::test_get_tree`
  asserts presence of `tree`/`root`/`widgets` only.

## Source-confirmed rulings and deliberate pins

* **Fail-closed bounds.** A malformed or out-of-range bound refuses with `INVALID_FIELD` rather than
  silently falling back to full depth; this mirrors the established `include_property_bindings`
  strictness already pinned by `Cortex.UMG.PropertyBinding.RefusesCoercedJsonTypes`.
* **One budget, one traversal.** `FTreeReadBudget` is passed by reference into the existing recursive
  builder; no second tree walk is introduced for the projection, and the count-only `total_widgets`
  traversal (pre-existing `CortexUMGUtils::CountWidgets`) is reused unchanged.
* **Honest projection.** `incomplete` is omitted (not `false`) on complete nodes to keep cut claims
  one-sided, and a null panel child can never make its parent look cut: `incomplete` is set only when
  the walk actually stopped early (`bChildrenCut`).
* **Ceiling reuse (corrected).** The combined-read refusal shares the module's single
  40000-character bounded-response ceiling (`MaxDetailedResponseChars`,
  `CortexUMGAnimationTrackUtils.h:72`) with the animation family rather than defining a second limit,
  but it measures the quantity that family does **not**: the compact condensed-writer text of the
  response. The animation/authoring reads keep their own pretty-estimate measurement; only the UMG
  tree branch was corrected. Because the decision is taken on the exact string that would be shipped,
  the refusal decision and the shipped envelope cannot disagree.

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` operation metadata and the generated capabilities cache
  still describe `umg.get_tree` without `depth`/`max_nodes`/`names_only` (and without the new
  `returned_widgets`/`tree_complete`/`reasons` response fields); regenerate from a live capabilities
  read. The native command info already advertises the three new optional params.
* The MCP `get_tree` tool (`MCP/src/tools/umg/widget_tree.py`) still takes `asset_path` only; the new
  bounds are reachable today through `umg_cmd`, so any signature snapshot stays unaffected until the
  tool surface is extended deliberately.
* Parent-repo `docs/systems/cortex-umg.md` and toolkit `resources/umg-patterns.md` /
  `templates/domains/umg.md` still describe the unbounded tree read; Task 17 owns their sync.
* The registered live `umg_cmd(get_tree)` shallow/names/full/cut/combined-bindings smoke and the
  zero-warning raw scans of that smoke are controller-owned and are **not** claimed here.
