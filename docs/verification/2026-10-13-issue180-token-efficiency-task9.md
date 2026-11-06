# Issue 180 — Token efficiency, Task 9: metadata-only Level component lists

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `6e7421c5d998c2a5746821418b04f192996808ed` (Task 4 completion)
**Owner:** Task 9 implementer (`ImplTask9LevelComponent`) — RED test staging, production
implementation and this record; all builds, test runs and smokes are controller-owned.

## Scope

`level.list_components` no longer expands reflected component property values by default. The
established up-to-20 editable-property window is available only when the caller opts in with
`include_properties=true`, which now also declares how complete that window is. Targeted
`level.get_component_property` remains the complete leaf read, and per-component identity metadata
(`name`/`class`/`is_root`/`creation_method`) is unchanged.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexLevel/Private/Operations/CortexLevelComponentOps.cpp` | `SerializeComponent` gains `bIncludeProperties`; it returns a metadata-only object **before any reflected iteration** when false; when true it counts every admitted editable property, serializes only the first 20 values and declares `total_property_count`/`returned_property_count`/`properties_complete`. `ListComponents` reads the optional `include_properties` and threads it into every component. |
| `Source/CortexLevel/Private/CortexLevelCommandHandler.cpp` | `list_components` operation metadata advertises the optional `include_properties` boolean. |
| `Source/CortexLevel/Private/Tests/CortexLevelComponentTest.cpp` | Consumer-visible regression `Cortex.Level.Component.ListComponentsPropertyMetadata` (staged before production; unchanged by this round). |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task9.md` | This record. |

Not part of this task (deliberately): the generated MCP fallback and capabilities fixtures, and the
parent-repo/toolkit Level documents — Task 17 owns those.

## Contract implemented

`level.list_components`

* New optional param `include_properties` (boolean, default `false`).
* Every component is still returned with `name`, `class`, `is_root` and `creation_method`, plus the
  actor-level `actor`, `components` and `count` fields.
* `include_properties=false` (default): no `properties` field and **no reflected property iteration
  at all** for the component summary.
* `include_properties=true`: `properties` holds the first up-to-20 admitted editable values — the
  identical iterator and predicate as before (`TFieldIterator<FProperty>` over the component class,
  admitted by `Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)`) — plus
  `total_property_count` (every admitted editable property), `returned_property_count` (`properties`
  field count, `min(20, total)`) and `properties_complete`
  (`total_property_count == returned_property_count`). Properties beyond the window are counted but
  never serialized.
* `level.get_component_property` is untouched: one named property, full value.

## Controller-observed RED (before implementation)

`Cortex.Level.Component+` (rendering runner): 10 tests, 9 PASS / 1 FAIL —
`Cortex.Level.Component.ListComponentsPropertyMetadata` with 8 consumer-visible assertion failures:
the default read wrongly expanded `properties`; `total_property_count` / `returned_property_count` /
`properties_complete` were absent (0 vs expected); the returned window count was unlabelled; and
`properties_complete` was not false under the cap. The admitted-editable count computed by the test's
own reflection predicate for the fixture root (`UStaticMeshComponent`) is **142** properties, so the
20-value window is capped and completeness must be false.
Log: `Saved/TestLogs/AutomationTest_2026-10-13_233650.log`.

The test's remaining assertions already passed under the previous production behaviour (single root;
root `name`/`class`/`is_root`/`creation_method`; the 20-name window and its ordering;
`ForcedLodModel == 0`; `bUseDefaultCollision == true`; a genuinely omitted admitted property; and the
targeted `get_component_property` values). The production change keeps the same iterator and
predicate, so the window content and those passing guards are unchanged.

## Controller GREEN (to be recorded)

Covering filter (serial, rendering runner, at most one runner at a time):

```
Cortex.Level.Component+
```

Expected: 10/10 GREEN, with a raw `Warning|Ensure|Fatal|Assertion` scan of the run log showing no
unexpected diagnostics. **This record does not claim GREEN** — the controller observes the covering
run and records its log identity here.

## Registered smoke (controller-owned)

`level_cmd` `list_components` against a task-owned actor: the default read returns identity metadata
with no `properties`; `include_properties=true` returns the capped window with truthful
`total_property_count` / `returned_property_count` / `properties_complete`; the omitted property is
recovered through `get_component_property`; then only that actor is deleted. Controller-owned, not
claimed here.

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` and `MCP/tests/fixtures/capabilities_cache_full.json`
  still describe `level.list_components` without `include_properties`; regenerate after a live
  capabilities read.
* Root `docs/systems/cortex-level.md` and toolkit `resources/{level-design,level-patterns,
  mcp-tool-reference}.md` still describe `list_components` as returning component properties;
  Task 17 owns them.
* The MCP `list_components` tool signature stays `(actor)` — the new opt-in is reachable through
  `level_cmd` — so `MCP/tests/fixtures/mcp_tool_signature_snapshot.json` is unaffected.
* No Python consumer asserts `properties` on `level.list_components`
  (`MCP/tests/test_level_e2e.py`, `MCP/tests/test_mcp_scenarios.py` read only identity/`count`), so no
  Python test changes are required by this contract.
