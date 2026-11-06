# Issue 180 — token-efficiency integrated acceptance record

Integrated candidate: plugin branch `feat/issue-180-token-efficiency`, 24 commits over the review base
`89611e195007e999febf9bd061de26ec01bc7988` (Task 3 completion), ending at the Task 17 integration and
graph-read-policy commits. Per-task records live beside this file as
`2026-10-13-issue180-token-efficiency-task{1..16}.md`. No push, PR, or merge was performed.

## Task coverage and independent review

| Task | Status | Per-task review |
|---|---|---|
| 1 Public output/metrics | complete | clean |
| 2 Discovery/toolsets | complete | clean (fix round 1) |
| 3 Explicit pagination | complete | clean (fix rounds 1–2) |
| 4 Asset selectors/search | complete | clean (fix round 1) |
| 5 Selected values (Data + Material) | complete | clean (fix round 1) |
| 6 Blueprint defaults discovery | complete | clean (fix round 1) |
| 7 Inherited delta + Core diff helper | complete | clean (fix round 1) |
| 8 Catalog sections + capability facade | complete | clean |
| 9 Level component inventory | complete | clean |
| 10 UMG tree bounds | complete | clean (fix round 1) |
| 11 Bounded log windows + QA coverage | complete | clean (fix round 1) |
| 12 Reflect lean views | complete | clean (fix round 1) |
| 13 Gen prompt gating | complete | clean (fix round 1) |
| 14 Class-default echoes | complete | clean (fix round 1) |
| 15 Offline catalog linking | complete | clean |
| 16 Frontend context dedupe | complete | clean |
| 17 Integration/acceptance | complete (one non-reproducible residual GC abort recorded) | pending integrated review |

Each task is a separate review package (`review-<base>..<head>.diff` under
`.superpowers/sdd/2026-10-13-token-efficiency-impl/`) with verdicts recorded in the matching
`task-N-review*.md`.

## Python marker matrix (integrated candidate)

| Marker | Command | Result |
|---|---|---|
| unit | `pytest tests/ -m "not e2e and not scenario and not stress"` | 1086 passed, 265 deselected (no editor) |
| e2e | `pytest tests/ -m e2e` (rendering editor) | 263 passed, 2 skipped, 1087 deselected |
| scenario | `pytest tests/ -m scenario` | 59 passed, 1293 deselected |
| stress | `pytest tests/ -m stress` | 14 passed, 1338 deselected |

The same non-e2e marker run while a live editor was up reports 4 additional `test_tcp_client.py`
port-discovery failures (`assert None is not None`); with no editor running it is 1086/1086, so those
are live-editor environment interactions, not regressions.

## Native acceptance

- Targeted families, each run serially after a rebuild: `Cortex.Data.SearchAssets+` 12/12,
  `Cortex.Data.DataAsset+` 14/14, `Cortex.Level.Component+` 10/10, `Cortex.UMG.TreeBounds+` 7/7,
  `Cortex.UMG.PropertyBinding+` 13/13, `Cortex.Core.Serializer+` 52/52, `Cortex.Blueprint.TokenRead+` 8/8,
  `Cortex.Blueprint.ClassDefaults+` 30/30, `Cortex.Blueprint.Discovery+` 6/6,
  `Cortex.Blueprint.ClassDefaults.Get+` 8/8, `Cortex.Reflect+` 63/63, `Cortex.Gen+` 54/54,
  `Cortex.Data.Catalog+` 7/7, `Cortex.Editor.Utility.LogCapture+` 7/7, `Cortex.Core.Capabilities+` 3/3,
  `Cortex.Blueprint+` 261/261, `Cortex.Frontend.InputArea.Resolution` 9/9 (rendering editor), and
  `Cortex.Graph.Authoring.Migration.Retire+` 55/55. Raw severity scans of every accepted run are clean.
- Full root command `RunTests.ps1 'Cortex+'`: the integrated candidate now reports **1875/1875, zero
  failures** (`Saved/TestLogs/AutomationTest_2026-10-14_045958.log`, raw severity scan clean), a 217 s
  rendering run. The pre-Task-17 attempt reported 1873/1875, the only two failures being capability-shape
  pins that Tasks 11/12 intentionally changed (`reflect.class_hierarchy` 5→6 params,
  `editor.get_recent_logs` 4→7 params); commit `45bc832d` migrates the two C++ pins and only the
  `editor.get_recent_logs` entry of the checked-in `mcp_tool_signature_snapshot.json` fixture. The rest of
  that fixture (for example the Python `get_class_defaults` signature) and the checked-in
  `capabilities_cache_full.json` were still stale at that commit; the follow-up fixture migration recorded
  below corrects them.

### Residual risk — two earlier full-suite attempts aborted in engine GC (not reproducible)

Between the migration and the final green run, two full `Cortex+` attempts aborted at a **Blueprint
compile** step with an engine crash, at a different test each time:

```
LogThreadingWindows: Error: Runnable thread Background Worker #0 crashed.
LogWindows: Error: Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x000000000000000c
  UE::GC::TBatchDispatcher<...>::FlushQueuedReferences()   GarbageCollection.cpp:3286
  UE::GC::TFastReferenceCollector<...>::ProcessObjectArray() FastReferenceCollector.h:967
  UE::GC::FRealtimeGC::CollectReferencesForGC<...>
```

Observed stops: `Cortex.Graph.Authoring.Migration.Retire.CompileRecoveryAcceptsExactErrorState`
(1206 tests seen) and `Cortex.Blueprint.Cleanup.RemoveSCSGuard.CompileFailureRollback` (117 tests seen).
The identical binary/candidate then passed `Cortex.Blueprint+` 261/261 and the retire family 55/55
standalone and finally completed the full root command 1875/1875, so the abort is not reproducible and
is recorded as a **non-blocking residual risk**, not a certified defect.

Unverified suspect — now triaged and discounted: Task 14 added `FCortexClassDefaultValueSnapshot`, the
only new `FGCObject` participant in this program (`CortexBPClassDefaultsOps.cpp:1201`, with
`AddReferencedObjects` at `:1256`). Source inspection shows no plausible causal link: the snapshot's
lifetime (constructed `:2428`, released with the journal at `:2533`) ends **before** the explicit
`FKismetEditorUtilities::CompileBlueprint` at `:2540` whose synchronous `CollectGarbage` is what walks the
crashing collector, and it is built only for wide-integer roots.

The evidence instead points to a pre-existing, environment-level intermittent engine GC crash reachable
from `CompileBlueprint` → `CollectGarbage`: 37 crash contexts with this exact signature (`0x…0c` at
`GarbageCollection.cpp:3286`) appear in this project's logs from 2026-02-12 through 2026-10-14 across UE
5.6.1 / 5.8.2 / 5.8.3, including a 2026-10-03 instance whose stack runs `SetReplicationSettings` →
`CompileBlueprint` → `CollectGarbage` with no Task 14 code anywhere in the path. Both 2026-10-14 aborts
stopped in tests that never construct a snapshot, and the snapshot-path tests passed inside both aborted
runs.

Conclusion: **non-blocking residual risk, not a defect of this program**; no code change is warranted on
the available evidence. One secondary latent item stays on watch and is recorded with its reproduction
recipe in `docs/tech-debt/cortex-blueprint-tech-debt.md`: `CollectObjectReferences` models every
`FObjectPropertyBase` uniformly and has no weak/lazy/interface/delegate branch.

## Live capabilities / fallback

On owned editor PID 46772 `scripts/sync_fallback.py` regenerated `_fallback_generated.py` from the live
cache and the output changed; that shows only that the committed fallback differed from that live cache, not
that the committed fixtures or fallback matched the verified registrations. `TestFallbackDrift` declares
`capabilities_cache_full.json` the fixture of record and that fixture intentionally omits `anim`, so the
fallback was regenerated with `--from-fixture`. That restored fixture↔fallback agreement, but agreement
between the two generated artifacts is not evidence that either reflects the registrations this program
changed — both fixtures were still stale for those commands.

The follow-up migration is therefore source-verified per entry instead:

- `mcp_tool_signature_snapshot.json` (the public Python tool signatures): the two entries whose registered
  Python function signature actually changed were re-derived from the function source:
  `blueprint.get_class_defaults` (`tools/blueprint/class_defaults.py`) gains `include_values`,
  `include_members`, `discovery_offset` and `discovery_limit` after the restored positional
  `blueprint_path`;
  `blueprint.cleanup_blueprint_migration` (`tools/blueprint/analysis.py`) gains `save`. Every other entry is
  byte-identical. `editor.get_recent_logs` deliberately keeps the seven-parameter native shape, because
  `Cortex.Core.Capabilities.Snapshot` pins that fixture entry to the native `editor.get_recent_logs`
  capability; the legacy Python wrapper (`tools/editor/utilities.py`) still takes only four arguments.
- `capabilities_cache_full.json` (a mirror of the native `GetSupportedCommands` payload): the entries whose
  native command metadata this program changed were updated from the owning `GetSupportedCommands`
  registration and cross-checked against the live `Saved/Cortex/capabilities-cache.json` —
  `data.search_assets`, `data.get_data_asset`, `data.get_data_catalog`, `blueprint.get_class_defaults`,
  `blueprint.list_inherited_properties`, `blueprint.list_settable_defaults`, `blueprint.set_class_defaults`
  (adds `echo`), `material.get_node`, `level.list_components`, `reflect.class_hierarchy`,
  `reflect.class_detail`, `editor.get_recent_logs` and `umg.get_tree`. `data.list_data_assets` was already
  current, `reflect.search` needed no change (Task 12 changed response metadata, not its parameters), and
  `gen.list_jobs` `include_prompt` is out of coverage because the fixture intentionally has no `gen` domain.
- `_fallback_generated.py` was then regenerated from the corrected fixture (`scripts/sync_fallback.py
  --from-fixture`), so the offline fallback publishes the same migrated signatures as the fixture.

## Post-acceptance: propagation and live MCP benchmark (2026-10-14)

> **Resolved (2026-10-14).** The five findings recorded in this section were re-verified and
> dispositioned: `umg.set_padding`, `umg.set_anchor` and `data.list_data_assets` were real and now
> refuse the undeclared parameter explicitly; the `core.get_capabilities` finding was an artifact of a
> **stale MCP server process** (it predated the `detail=names` facade), not a code defect; and the
> `material.create_material` finding did not reproduce and was withdrawn. See
> `2026-10-14-mcp-benchmark-remediation.md`.

**Toolkit propagation.** `cortex-toolkit` branch `feat/issue-180-contract-sync` off `fb5f959`, 9
documentation-only commits (tip `fda9c4750321f717d0d49b55c1b1845f41533258`), not pushed. It updates
resource/agent guidance for the changed contracts and corrects one contract-matrix claim from source: the
component `creation_method` enum has five values (`Native`, `SimpleConstructionScript`,
`UserConstructionScript`, `Instance`, `Unknown`), not three
(`Source/CortexLevel/Private/Operations/CortexLevelComponentOps.cpp:169-184`). Its report flags, without
fixing, the compatibility gaps it found: the standalone `get_recent_logs` tool cannot pin a window;
`query_class_hierarchy` drops native `truncated`; `changed_only`/`echo` are router-only; and
`list_data_assets` silently ignores the retired `class_filter` (confirmed live — see below).

**Documentation sync.** Parent-repo commit `abd4851`: `docs/systems/{INDEX,cortex-blueprint,cortex-data,
cortex-editor,cortex-gen,cortex-level,cortex-material,cortex-qa,cortex-reflect,cortex-umg,python-mcp}.md`,
`docs/requests/{INDEX.md,core/2026-04-07-large-response-handling.md}` (status: implemented and verified
locally, unpublished) and `docs/tech-debt/{cortex-blueprint,cortex-mcp}-tech-debt.md` (`TD-BP-GC-001` for
the residual GC risk above, `TD-MCP-004` for the fixture divergence). The coverage gaps the contract matrix
identified — blueprint class-defaults, editor log windows, level component properties, material node
properties, UMG tree bounds and QA `log_coverage` — now have homes in their owning system docs.

**Live MCP benchmark.** Executed against plugin 0.3.7 / engine 5.8.3 (project `CortexSandbox`), 17 of 18
checklist groups run end-to-end through the running editor, plus targeted probes of every contract this
program changed. Verified live and correct: complete-or-refuse refusals with explicit envelopes; paging
metadata (`data.search_assets` `next_offset`/`is_complete`; `editor.get_recent_logs` cursor family with
`lost_before_cursor`; `umg.get_tree` `total_widgets`/`returned_widgets`/`tree_complete`/`reasons`);
`level.list_components` property inventory (`returned_property_count` 20 of `total_property_count` 85,
`properties_complete: false`); `reflect.class_hierarchy` flat `classes` + `truncated`; `reflect.class_detail`
`summary` omitting properties/functions; blueprint class-defaults discovery/delta/echo surfaces; the
migration V2 schema; string-table dry-run and `string_table_refs` scan at `limit: 25` without router-side
truncation; and `generate_project_schema` (`errors: []`, 2.39 s). Not executed: the PIE lifecycle checks
(`TestMap` configures no game mode) and the human visual-confirmation step.

Five issues were logged, none of them a regression in a contract this program declares. One sits on the
program's changed path and is worth fixing before publication: **`core.get_capabilities` is unreadable
through the MCP surface** — every variant (default, `detail: "names"`, `detail: "full"`, and even an
*invalid* `detail`, which must return `INVALID_FIELD`) returns the generic 40 KB refusal
`{"_error": "RESPONSE_TOO_LARGE", "_size": 157795, "_suggestion": "Pass 'limit' parameter…"}`, and
`limit` is not a parameter of that command. Because the invalid-`detail` case never reaches the handler,
the Python `detail=names` projection (`MCP/src/cortex_mcp/tools/routers.py:_get_capabilities`) is being
bypassed by the size guard that `MCP/src/cortex_mcp/response.py` applies first — both halves of that
interaction are this program's work, and neither the native nor the Python suite drives
`core_cmd(get_capabilities)` end to end, which is why the suites stayed green. The other four are outside
the changed contracts: `umg.set_padding` and `umg.set_anchor` report `set: true` while ignoring the
requested value (Medium), `data.list_data_assets` silently ignores the retired `class_filter` and returns
unfiltered results (Medium), and `material.create_material` returns an `asset_path` its sibling commands
reject (Low). Full evidence and fix hints: `Saved/mcp_benchmark_issues_20261014_075611.md`.

## Deferred to the remaining Task 17 tail

- Triage of the five benchmark issues above — in particular the `core.get_capabilities` guard-ordering
  defect, which should be fixed before any publication. This is new, unfixed work; the program's
  acceptance does not depend on it.
- Publication: no push, pull request, merge or branch deletion was performed for any of the three
  repositories. The endpoints are the local branch `feat/issue-180-token-efficiency` (plugin, tip
  `795ababf`), the local branch `feat/issue-180-contract-sync` (toolkit, tip `fda9c47`) and the parent-repo
  commit `abd4851`; the parent-repo submodule pointers remain uncommitted, as do the pre-existing
  unrelated modifications to `.agents/skills/handling-github-issues/*`, `.omp/config.yml` and
  `.cortex/schema/*`.
