# Issue 255 verification: declared params enforced for every routed command

**Date:** 2026-11-19
**Request:** <https://github.com/etelyatn/UnrealCortex/issues/255>
**Design / impl:** `docs/plans/2026-11-19-issue255-declared-params-enforcement-{design,impl}.md`
**Base:** `origin/main` `eae47079`, UE 5.8, Win64 MSVC Development editor build
**Evidence:** CortexSandbox `Saved/Issue255/` (suite console output, copied automation logs, pytest output, benchmark driver output)

## Acceptance

| Criterion | Evidence | Result |
|---|---|---|
| Every routed command refuses an undeclared param, and the clear unknown-field error names the field | `Cortex.Core.Router.DeclaredParams.*`, 4 tests. The RED run failed all 4 before the router change and the GREEN run passed after it. Live TCP probes on the candidate editor are listed below this table. | Pass |
| No declaration/handler mismatch remains | Six per-domain source audits covered all 14 handlers, about 340 commands, in two directions: keys read but not declared were added, and keys declared but never read were removed. A dispatched-name scan found one undeclared alias, `graph.authoring_context`, which was removed. A review follow-up removed the `gen.start_*` keys that no provider consumes. The full native suites, Python suites and live benchmark ran with enforcement on, and no unknown-param refusal came from a real caller. | Pass |
| Native suites, Python tests and the MCP benchmark pass with enforcement on | See the native and Python tables below. The live benchmark (Checks 1-16 and 18, through a real `cortex_mcp` server over stdio) found no enforcement defects. | Pass |
| Underscore keys exempt | `_telemetry_parallel` was accepted live and in the native test | Pass |

### Live TCP probes (candidate editor, `project_name` CortexSandbox, port 8742)

| Request | Result |
|---|---|
| `editor.get_recent_logs {bogus_one, bogus_two}` (#234 item 1) | `INVALID_FIELD` `Unknown param 'bogus_one'. Accepted params: severity, since_seconds, since_cursor, until_cursor, limit, count_only, category` |
| `editor.get_recent_logs {limit, _telemetry_parallel}` | success |
| `data.list_datatables {<200-char key>}` | `INVALID_FIELD` `Unknown param (name length 200). ...` |
| `core.list_redirectors {}` (StrictParams) | `INVALID_FIELD` `Missing required param: path (string)` |
| `graph.apply_patch {..., graph_name}` | `INVALID_FIELD` naming `graph_name`, with the compact patch outcome in `details` (`patch_id`, `apply_status`, ...) |
| `batch [data.list_datatables {oops}, data.list_datatables {}]` | step 0 `INVALID_FIELD`, step 1 success |
| `graph.authoring_context` | `UNKNOWN_COMMAND` (alias removed) |

## Native suites (candidate build, enforcement on)

| Suite | Found | Passed |
|---|---|---|
| Cortex.Core+ | 245 | 245 |
| Cortex.Data+ | 156 | 156 |
| Cortex.Graph+ | 368 | 368 |
| Cortex.Blueprint+ | 406 | 406 |
| Cortex.UMG+ | 183 | 183 |
| Cortex.Reflect+ | 106 | 106 |
| Cortex.Animation+ | 56 | 56 |
| Cortex.StateTree+ | 65 | 65 |
| Cortex.Gen+ | 55 | 55 (re-run after the gen declaration correction) |
| Cortex.Level+ (rendering) | 79 | 79 |
| Cortex.Editor+ (rendering) | 189 | 189 |
| Cortex.QA+ (rendering) | 20 | 20 |
| Cortex.Replay+ (rendering) | 177 | 177 |

The only raw warning in the runs was `LogHttp` "request timed out" on the engine's
`https://www.google.com/generate_204` connectivity probe, which appeared twice during StateTree. It is
host network state and does not come from Cortex code.

`Cortex.Blueprint+` exceeds the default 120 s whole-run budget of `RunTests.ps1`. One run was cut off
inside `CompileWarningContract` and appeared as a hang. A re-run with `-Timeout 1800` executed all 406
tests.

Four tests that relied on the removed per-handler validation were moved onto the router path, the
same path TCP callers use:

- `Cortex.Blueprint.StrictParams.DeclaredContractEnforced`
- `Cortex.Blueprint.ReplaceInterface.Basic`
- `CortexReflectDuplicateGraphTest`
- `CortexReflectNameLookupTest`

Expected parameter counts and lists were updated for the corrected declarations in
`Cortex.Core.Capabilities.RepresentativeMetadata` and `Cortex.Core.Capabilities.Snapshot`.

## Python (`Plugins/UnrealCortex/MCP`)

| Set | Result |
|---|---|
| `not e2e and not scenario and not stress` | 1290 passed |
| `e2e` (live editor) | 263 passed, 2 skipped after two fixes; the first run showed 6 failures |
| `scenario` | 59 passed after the same fixes |
| `stress` | 14 passed |

The e2e and scenario fixes:

- `test_stress_bulk_scene_composite` sent the stale `class_filter` to `level.list_actors`. Before this
  change it was silently ignored and the test passed against an unfiltered count. It now uses the
  declared `class`. This is exactly the defect class issue 255 targets.
- `test_scenario_retire_entries_live_workflow` sent `cleanup_migration {compile: false}` without
  `save: false`. The handler refuses that combination with `save=true requires compile=true`. That
  guard, the request and the router outcome are all unchanged by this work, so this was existing test
  drift. The test now sends `save: false`.

## Live MCP benchmark

The benchmark ran Checks 1-16 and 18 against the candidate editor through a stdio `cortex_mcp` server.
The session MCP connection was unavailable. The driver output is in
`CortexSandbox/Saved/Issue255/bench/`, and the issue log is
`CortexSandbox/Saved/mcp_benchmark_issues_20261119_111355.md`.

- **Outcome:** every check passed. Check 6 gathered its data but did not print C++. Check 10b skipped
  the reference scan because no benchmark references exist. Check 17 was replaced by agent inspection
  of the screenshot: a red cube in frame.
- **Enforcement defects:** none. No param sent by a real MCP tool or composite was refused.
- **Stale checklist param:** the checklist's `material.add_node {params: {...}}` was refused, which is
  the intended behaviour. That call now uses `add_node` followed by `set_node_property`.
- **Unrelated observations:** two were logged without action.
  - `editor.focus_actor` does not accept an actor's internal object name as `actor_path`.
  - `BP_ComplexActor` reports no timelines.

## Static and portability gates

- **Clang syntax gate:** `cli/clang/clang-syntax-check.py` passed 296 changed units with 0 errors and
  0 warnings. The later gen declaration edit was re-checked with an explicit-path run.
- **Capability mirror:** `MCP/tests/fixtures/capabilities_cache_full.json` commands were regenerated
  from the live `Saved/Cortex/capabilities-cache.json`, and `_fallback_generated.py` from that fixture
  (`scripts/sync_fallback.py --from-fixture`).

## Limits

- Native tests that call `Handler.Execute` directly bypass the router by design. The router-path
  tests, the e2e and scenario suites and the benchmark are the consumer-facing proof.
- The declaration audit is source review. Declaring a key does not prove that every provider consumes
  it. One such gap was found in review (`gen.start_*`) and corrected.
