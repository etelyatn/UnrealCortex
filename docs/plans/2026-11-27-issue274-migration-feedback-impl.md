# Issue 274 implementation plan

**Goal:** deliver every item of [UnrealCortex#274](https://github.com/etelyatn/UnrealCortex/issues/274) per
`2026-11-27-issue274-migration-feedback-design.md` (the spec; read it first).

**Execution:** six parallel implementation slices with exclusive file ownership on branch
`fix/274-migration-feedback`, then one integration build, RED/GREEN automation runs, Python tests, Clang gate,
live editor smoke, review, and themed commits. Triage evidence with file:line citations:
`CortexSandbox/Saved/Issue274/triage-*.md`.

## Global constraints

- UE 5.8, Epic coding standard, `docs/unreal-coding-standards.md`. Game thread only. `FScopedTransaction` on writes.
- Saves use `SAVE_NoError`. Logging: module categories, never `LogTemp`.
- New params/commands declared in `GetSupportedCommands` with exact descriptions; enums list their values.
- Tests: `IMPLEMENT_SIMPLE_AUTOMATION_TEST` under the module namespace, never reuse an existing full test name as a
  prefix of a new one (leaf-only execution). Fixture suffixes from `FGuid::NewGuid().ToString().Right(8)`. Created
  assets marked garbage at cleanup. Zero warnings.
- No builds and no test runs by implementers (the controller builds once). Python slice may run pytest with
  `uv run --no-sync` and `--basetemp=D:/UnrealProjects/CortexSandbox/Saved/task/pytmp`.
- Do not edit `capabilities_cache_full.json`, `_fallback_generated.py`, `CortexSandbox/docs/`, version files.
- Do not commit. The controller commits per slice.

## Slices and file ownership

| Slice | Items | Owns |
|---|---|---|
| A | 1 3 4 5 7 8 14 20 | `MCP/src/tools/editor/composites.py`, `MCP/src/cortex_mcp/tools/standalone/editor.py`, `MCP/tests/test_editor_lifecycle.py`, `Source/CortexCore/**`, `Source/CortexEditor/**`, `README.md` |
| B | 9 11 12 13 15 16(nodes) 17 | `Source/CortexGraph/**`, `Source/CortexBlueprint/Private/Operations/CortexBPGraphMetricsOps.*` and its tests, `MCP/src/cortex_mcp/response.py` and its tests |
| C | 16(graphs) 18 | `CortexBPRemoveGraphOps.*`, `CortexBPAdoptMemberOps.*` and their tests; `remove_graph`/`adopt_native_member` entries in `CortexBPCommandHandler.cpp` |
| D | 6 10 19 | `Source/CortexQA/**`, `Source/CortexReflect/**`, `CortexUMGWidgetTreeOps.cpp` + rename tests; `rename_widget` entry in `CortexUMGCommandHandler.cpp` |
| E1 | 2 21 E-c E-d | `CortexBPReparentOps.*`, `CortexBPGuardedRun.*`, `CortexBPClassDefaultsOps.*`, new `CortexBPEmbeddedTemplates.*`, their tests; `reparent`/`set_class_defaults` entries in `CortexBPCommandHandler.cpp` |
| E2 | E-a E-b | `CortexUMGEmbeddedValueOps.*`, embedded test files/types; embedded entries in `CortexUMGCommandHandler.cpp` |

Shared handler files (`CortexBPCommandHandler.cpp`: C, E1; `CortexUMGCommandHandler.cpp`: D, E2): re-read right
before each edit and touch only your own command entries and dispatch lines.

## Per-slice acceptance (tests to add)

Each slice adds automation tests that fail on `main` and pass with the change, through the public command route
(`FCortexCommandRouter`/handler JSON), for the positive behavior and the refusal named in the spec:

- A: Python `test_editor_lifecycle.py` cases for engine resolution from the running exe, refusal when the old pid
  is unknown, refusal when the port file pid != launched pid, `force` passthrough and `UNSAVED_CHANGES` passthrough.
  Native: `Cortex.Core.DirtyPackages.*` (dirty a transient-free test package → listed; after save → absent),
  `Cortex.Core.Shutdown.RefusesDirty` (no exit armed; use the handler without the ticker firing),
  `Cortex.Editor.StartPIE.RejectsUnknownMode`, `Cortex.Editor.ConsoleCommand.EditorWorld` (`stat unit` without PIE
  succeeds via `engine_exec`), `Cortex.Editor.RecentLogs.Contains`, `Cortex.Editor.CaptureScreenshot.Source*`
  (`source: pie` without PIE refuses; `source` validation).
- B: patch with `call_kind: interface_message` yields `K2Node_Message`; serialization of CreateDelegate and
  branch VariableGet; metrics conditional count; prune with a knot and with `include_result_producers`;
  `remove_nodes` by list and by keep-set; `result` locator + `disconnect`; `get_subgraph` `out_path` file content.
- C: `remove_graph names[]` preview hash, atomic apply, stale-hash refusal; adopt non-visible already-adopted.
- D: `look_at` with a camera offset (PIE) aims camera→target; reflect `truncated` flags; rename label fields.
- E1: failed reparent restores the CDO and lists `reload_packages`; plan `type_changing_call_sites`;
  `set_class_defaults` on an embedded widget class reports and (opt-in) resyncs only non-overridden instances;
  reparent dropping a property reports `dropped_overrides`.
- E2: diff lists defaults-only native property differences; values include defaults-only with `instance_editable`.

## Integration (controller)

1. Regenerate capabilities fixture and fallback; Python suite (non-e2e).
2. RED build: production sources at `main`, new tests in place; run new tests; record failures.
3. GREEN build; run affected namespaces (`Cortex.Core+`, `Cortex.Editor+`, `Cortex.Graph+`, `Cortex.Blueprint+`
   with `-Timeout 1800`, `Cortex.UMG+`, `Cortex.QA+`, `Cortex.Reflect+`).
4. Clang syntax gate; live editor smoke of each item through MCP.
5. Review, docs (`CortexSandbox/docs/systems/`), version bump, toolkit sync, PR, merge.
