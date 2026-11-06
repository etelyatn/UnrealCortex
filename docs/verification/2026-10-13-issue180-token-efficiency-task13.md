# Issue 180 — Token efficiency, Task 13: Gen job lists without repeated prompt bodies

**Date:** 2026-10-13 (record written 2026-10-14)
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `7b62ef4` (plugin HEAD at RED staging / GREEN implementation)
**Owner:** Task 13 implementer (`ImplTask13Gen`) — production implementation and the SDD-stage
report; the RED staging test was authored in the prior task-13 stage (`Task13GenJobs`). All builds,
test runs and smokes are controller-owned.

## Scope

`gen.list_jobs` no longer repeats every job's prompt body in the row payload. The body is emitted
only when the caller opts in with `include_prompt=true`. Job identity/metadata and the top-level
count stay complete, `job_status` still returns the full prompt, and the native `ListJobs`
limit/filter semantics — including `limit=0` meaning unlimited — are untouched.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexGen/Private/CortexGenCommandHandler.cpp` | `HandleListJobs` parses the optional `include_prompt` bool (default `false`) and emits the per-row `prompt` field only when it is `true`; `GetSupportedCommands()` advertises the optional `include_prompt` boolean on the `list_jobs` entry. |
| `Source/CortexGen/Private/Tests/CortexGenJobManagerTest.cpp` | Consumer-visible regressions (`Cortex.Gen.CommandHandler.ListJobsPromptEmission`, `…ListJobsNativeLimitSemantics`, `…JobStatusPromptRetained`) staged in the preceding RED round; unchanged by this round. |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task13.md` | This record. |

Not part of this task (deliberately): the generated MCP fallback/capabilities fixtures and the
toolkit/parent-repo Gen documents — Task 17 owns those. The user-facing
`docs/systems/cortex-gen.md` row edit (parent repo) stays local and unpushed for Task 17 to publish.

## Contract implemented

`gen.list_jobs`

* New optional param `include_prompt` (boolean, default `false`), read with the plugin-wide guarded
  optional-bool convention `Params->TryGetBoolField(TEXT("include_prompt"), bIncludePrompt)`; it
  stays `false` when the field is absent, `null`, or not a JSON boolean, so no coercion can turn the
  body on implicitly.
* `include_prompt=false` (default) and explicit `false`: every row omits the `prompt` field
  entirely — **absence, not an empty string**.
* `include_prompt=true`: each row's `prompt` equals the canonical
  `FCortexGenJobManager::GetJobState(job_id)->Prompt`.
* Unchanged per row: `job_id`, `status`, `provider`, `progress`, `created_at`, `completed_at`.
  Unchanged per response: `jobs`, `count`.
* Unchanged native behavior: `status`/`limit` are still forwarded verbatim into
  `JobManager->ListJobs(StatusFilter, Limit)`; `limit` omitted or `0` remains native-unlimited
  (never reinterpreted as a cached-pagination request) and a positive `limit` keeps the existing
  bounded native page.
* `gen.job_status` is untouched: it still returns the full `prompt` regardless of the `list_jobs`
  default.
* No provider, network, polling-cadence, download/import, persistence (`LoadJobs`/`SaveJobs`) or
  job-lifecycle change.

## Controller-observed RED (before implementation)

`Cortex.Gen+` (rendering runner): 53 PASS / 1 FAIL — the single failure is
`Cortex.Gen.CommandHandler.ListJobsPromptEmission`: the default `list_jobs` row wrongly carried the
prompt body, and explicit `include_prompt=false` likewise failed to omit it on every row. The two
guards (`…ListJobsNativeLimitSemantics`, `…JobStatusPromptRetained`) and the pre-existing
`Cortex.Gen.*` suites passed.
Log: `Saved/TestLogs/AutomationTest_2026-10-14_004053.log`.

## Controller-observed GREEN (after implementation)

Covering filter (serial, rendering runner, at most one runner at a time):

```
Cortex.Gen+
```

Result: **54/54 GREEN**, raw `Warning|Ensure|Fatal|Assertion` scan of the run log clean.
Log: `Saved/TestLogs/AutomationTest_2026-10-14_010459.log`.

## Live smoke / availability limitation

The brief requires live `gen_cmd` `list_jobs`/`job_status` evidence only if live Gen is enabled.
Task 3 already recorded a fresh `tools/list` with `gen_cmd` **unavailable**
(`docs/verification/2026-10-13-issue180-token-efficiency-task3.md`), so the honest outcome is the
recorded absence of the domain plus this native handler evidence. No live-provider pass is claimed
anywhere in this record.

## Task 17 handoff (not done here)

* Generated MCP fallback/capabilities fixtures describing the Gen schemas must be regenerated after
  a live capabilities read so `list_jobs` advertises `include_prompt`; no consumer in
  `Plugins/UnrealCortex/MCP` reads a row `prompt` from `gen.list_jobs` (the only Gen entry is the
  `("gen", "list_jobs")` native-passthrough registration in `MCP/src/cortex_mcp/tools/routers.py`
  and `MCP/tests/test_pagination_contract.py` uses `{"job_id": "j1"}` only), so no Python test change
  is required by this contract.
* Parent-repo `docs/systems/cortex-gen.md` (command-table `list_jobs` row now notes prompts are
  omitted unless `include_prompt`=true) and any toolkit Gen resources remain Task 17's publication
  scope; the edit stays local/unpushed here.
