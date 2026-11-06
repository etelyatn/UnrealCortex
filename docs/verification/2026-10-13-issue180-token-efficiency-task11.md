# Issue 180 — Token efficiency, Task 11: canonical bounded logs and complete finite QA windows

**Date:** 2026-10-14
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Owner:** Task 11 implementer — RED staging, production implementation and this record; all builds,
test runs, smokes and any commit are controller-owned.

## Scope

`editor.get_recent_logs` returns a finite, self-describing window instead of an unbounded, aliased
entry list. The QA log consumers read that window through one collector that never presents a
partial window as complete, and both detector copies read the canonical `entries`/`severity` shape.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexEditor/Private/CortexEditorLogCapture.h` | `FCortexEditorLogResult` gains `NextCursor`/`LatestCursor`/`WindowEndCursor`/`OldestAvailableCursor`/`MatchedCount`/`bHasMore`/`bLostBeforeCursor`; the `Cursor` alias is gone. `GetRecentLogs` gains `Limit = 200`, `UntilCursor = INDEX_NONE`, `bCountOnly = false`. New private `EvictedThroughCursor`. |
| `Source/CortexEditor/Private/CortexEditorLogCapture.cpp` | Ring eviction records the highest evicted cursor; `GetRecentLogs` scans once under the existing buffer lock, fixing the window end, counting matches, retaining at most `Limit`, and computing the continuation without consuming an omitted match. |
| `Source/CortexEditor/Private/Operations/CortexEditorUtilityOps.cpp` | Reads/validates `limit` (1..2000), `until_cursor`, `count_only` and `since_seconds` (nonnegative, or exactly `-1` to disable); refuses malformed durations/limits with `INVALID_FIELD`; serializes only `entries` plus the window metadata. |
| `Source/CortexEditor/Private/CortexEditorCommandHandler.cpp` | `get_recent_logs` metadata advertises `limit`/`until_cursor`/`count_only`, documents exactly `-1`, and corrects the `since_cursor` type to `number`. |
| `Source/CortexEditor/Private/Tests/CortexEditorUtilityTest.cpp` | Four staged RED regressions plus the two pre-existing tests migrated off the `Cursor` alias; a fix-round regression asserts the near-`-1` negative durations refuse while exactly `-1` disables. |
| `MCP/src/tools/qa/composites.py` | New `_log_window_baseline` (count-only pre-action watermark returning `(start_cursor, failure)`) and `_read_log_window` (finite collector on the one-shot transport); `test_step`/`run_scenario_inline` use them, carry the previous window end forward across scenario steps, and retain per-step `log_coverage`. |
| `MCP/src/tools/qa/detector.py`, `MCP/src/cortex_mcp/qa/detector.py` | Consume `entries`/`severity` (lowercase `error`); drop the `logs`/`level` shape and its textual `Error:` heuristic. Both copies remain byte-identical. |
| `MCP/tests/test_qa_tools.py` | 9 staged/migrated RED regressions and the finite-window double, plus four fix-round regressions: late-record carry-forward attribution, distinct baseline-failure coverage, one-shot-transport assertions, and the transport-aware double. |
| `MCP/tests/test_editor_e2e.py` | `test_get_recent_logs` migrated off the removed `cursor` alias onto the window metadata. |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task11.md` | This record. |

Controller-owned and deliberately not edited here: the `("editor", "get_recent_logs")` entry in
`MCP/src/cortex_mcp/tools/routers.py`, and the generated MCP fallback/capabilities fixtures
(Task 17 / a live cache owns those).

## Native contract

`editor.get_recent_logs` params: `severity` (min severity, default `log`), `since_seconds` (default
`30.0`; **exactly** `-1` disables time filtering, and every other negative — including values inside
the float tolerance of `-1` — refuses `INVALID_FIELD`), `since_cursor` (exclusive lower bound,
default `-1` = from the beginning), `until_cursor` (inclusive window end pin, optional), `limit`
(`1..2000`, default `200`), `count_only` (boolean), `category`.

Result `data`:

* `entries` — retained matching rows only (`cursor`/`timestamp`/`category`/`message`/`severity`),
  canonical lowercase `severity` (`error`/`warning`/`log`). No `logs` duplicate array, no `cursor`.
* `count` — retained row count (`0` for `count_only`).
* `next_cursor` — the continuation start: the last consumed match cursor when the page was capped,
  or the window end when the scan finished (so a nonmatching tail is advanced past, and a match the
  cap omitted is never consumed).
* `latest_cursor`, `window_end_cursor`, `oldest_available_cursor`, `matched_count` (all matches in
  the window whether or not they fit), `has_more`, `lost_before_cursor`.

The window end is the caller's `until_cursor` clamped to the newest retained row, or the newest row.
Rows past it are never attributed to the window, so a record added after a pinned end belongs to the
next read. Time filtering anchors to the retained end timestamp of the window. Loss is reported only
when the row at the requested start was actually evicted from the front of the ring buffer.

## QA consumer contract

`_read_log_window(connection, start_cursor, baseline_failure=None) -> (entries, coverage)` reads one
action's window: a first page that fixes the end-of-step watermark, then pinned continuations while
`has_more` and not `lost_before_cursor`. `coverage` carries `complete`, `lost_before_cursor`,
`window_end_cursor`, `count` and `cause`. `complete` is false for front eviction, a native/MCP size
refusal (`_error`), a connection failure, non-advancing pagination, a failed or unavailable watermark,
or the page-cap guard — always with the entries read so far and an explicit cause. Every read uses the
one-shot `send_command_once` transport, so a first failure yields incomplete coverage immediately
instead of a rediscovery/sleep/resend retry; the helper never polls, never retries and never
overwrites an applied action's success.

A single pre-action count-only watermark (`_log_window_baseline`, returning `(start_cursor, failure)`)
bounds the **first** step's window; every later step continues from the previous window's fixed end,
so a record that arrives after a fixed end is attributed to exactly the next action rather than
falling between two baselines. A failed watermark is reported as its own cause instead of `-1` (which
would read the whole buffer and misattribute every retained record to the current action).

## Verification (controller-owned)

* Native filter: **`Cortex.Editor.Utility.LogCapture+`**.
* Python: **`MCP/tests/test_qa_tools.py`**.

Observed RED before implementation (recorded by the controller): native 2 pass / 4 fail
(`ContinuationConsumesOmittedMatchesExactlyOnce`, `EmptyWindowContinuationNeverRegresses`,
`FrontEvictionIsExplicit`, `LateRecordWaitsForNextWindow`, `032128.log`); python 9 failed / 9 passed.
The GREEN run, the live `editor_cmd` log stream and the `scenario_compose`/`qa_test_step` smoke are
recorded by the controller.
