# Issue 180 — Token efficiency, Task 6: bounded metadata discovery and single struct representation

**Date:** 2026-10-13
**Repository:** `Plugins/UnrealCortex` (plugin repo; `MCP/` is part of this repo)
**Branch:** `feat/issue-180-token-efficiency`
**Base revision:** `7b62ef4` (Task 5 fix; `HEAD` at staging and at implementation)
**Owner:** Task 6 implementer (`ImplTask6Bp`) — RED test staging, production implementation and this
record; all builds, test runs and smokes are controller-owned.

The RED tests were staged while Task 5's changes were still uncommitted on top of `f27c48b`; the
controller observed RED against that tree and GREEN after Task 5 landed as `7b62ef4`.

## Scope

The three Blueprint class-default reads stop extracting reflected values by default. Metadata
discovery is now the default for an unfiltered read; values, struct members and the discovery page
window are explicit opt-ins with truthful metadata.

Changed files (this task):

| File | Change |
|---|---|
| `Source/CortexBlueprint/Private/Operations/CortexBPClassDefaultsOps.cpp` | Shared read-control parsing/refusal, one shared property/struct builder, shared `ListDefaults` for the two list reads, reworked `GetClassDefaults`, bounded metadata paging. ~180 duplicated lines deleted. |
| `Source/CortexBlueprint/Private/Operations/CortexBPClassDefaultsOps.h` | Private `ListDefaults` declaration; read-contract doc comments for the three reads. |
| `Source/CortexBlueprint/Private/CortexBPCommandHandler.cpp` | The three read commands advertise `properties`, `include_values`, `include_members`, `discovery_offset`, `discovery_limit`. |
| `MCP/src/tools/blueprint/class_defaults.py` | `get_class_defaults` accepts and conditionally forwards the four controls; docstring documents metadata-only discovery as the default. |
| `Source/CortexBlueprint/Private/Tests/CortexBPTokenReadTest.cpp` | New consumer-visible regression file, 5 tests (staged before production; unchanged by this round). |
| `Source/CortexBlueprint/Private/Tests/CortexBPDiscoveryOpsTest.cpp` | Two migrated explicit-value reads + `Defaults.MetadataOnlyWithoutValues` + `Defaults.InvalidReadControlsRefuse` (staged; unchanged). |
| `Source/CortexBlueprint/Private/Tests/CortexBPClassDefaultsTest.cpp` | `Get.NestedDiscovery` migrated to request values/members explicitly (staged; unchanged). |
| `MCP/tests/test_class_defaults.py` | Discovery-omits-values guard + explicit values/members/paging forwarding test (staged; unchanged). |
| `Plugins/UnrealCortex/docs/verification/2026-10-13-issue180-token-efficiency-task6.md` | This record. |

Not part of this task (deliberately): the generated MCP fallback and capabilities fixtures, the
toolkit/parent-repo documents — Task 17 owns those.

## Contract implemented

All three reads — `get_class_defaults`, `list_settable_defaults`, `list_inherited_properties` — gain the
same controls (`properties`, `include_values`, `include_members`, `discovery_offset`, `discovery_limit`).

* **Parsing/refusals happen first**, before any property is admitted and before any asset load or
  Blueprint compile: a present field that is not a JSON boolean (`include_values`, `include_members`) or
  not an integral JSON number in range (`discovery_limit` 1..200, `discovery_offset` >= 0) refuses with
  `INVALID_FIELD`; a present `properties` that is not an array, or an array element that is not a string,
  refuses the same way (it never degrades to an unfiltered read). Field presence/type is read with
  `FJsonObject::TryGetField` / `HasField`.
* **`include_values` default is context-dependent**: a nonempty `properties` selector on
  `get_class_defaults` defaults to `true` (effective-value read); an unfiltered read and both list reads
  default to `false` (metadata discovery). An explicit value always wins, so an explicit
  `include_values=false` keeps a selected read metadata-only. `values_included` truthfully declares the
  mode in every response.
* **`include_members` is opt-in** (default `false`). When true, a struct property carries a one-level
  `members` map of `type` and dot-notation `path`; member `value` is extracted only when
  `include_values` is also true. Without members the struct entry has no `members` field at all.
* **Metadata paging**: an unfiltered metadata read returns one bounded page of the admitted population
  sliced from `discovery_offset` (default `0`) with `discovery_limit` (default `100`, max `200`) and
  declares `total_properties` plus `discovery_pagination{offset,limit,returned,next_offset,is_complete}`.
  `next_offset` is `offset + returned` while incomplete and null once the page reaches the population
  end; completeness is only claimed when the returned page actually reaches it. No page size is ever
  fabricated before extraction — the population is counted, not inferred.
* **Selected value reads are never paged**: `get_class_defaults` with nonempty `properties` ignores
  `discovery_offset`/`discovery_limit` (the values survive `offset=1, limit=1`), returns exactly the
  requested paths and preserves every effective value including `false`/zero/null. Struct leaf selectors
  (`PrimaryActorTick.bCanEverTick`) return that leaf's value keyed by the requested path.
* **List-read scope is enforced before leaf resolution**: `list_settable_defaults` admits
  `CPF_Edit|CPF_BlueprintVisible` (minus deprecated/`DisableEditOnTemplate`) supported roots;
  `list_inherited_properties` additionally excludes properties owned by the Blueprint's own generated
  class. A selector whose root is not admitted refuses with `PROPERTY_NOT_FOUND` instead of silently
  listing the whole population. Both list reads now also accept `properties` selection.
* **Preserved metadata**: `type`, `category`, `defined_in` and discovery `accepted_formats` stay on every
  entry; FText values keep their `string_table` sibling metadata when values are included.
* **Single shared struct representation**: all three reads build entries through one
  `BuildDiscoveryPropertyInfo` and one `BuildStructMembersInfo`, so struct/member JSON is identical
  across discovery, list and selected reads (previously `get_class_defaults` had its own inline builder
  and its own member block).

Response shape additions are additive: `values_included` on every response, `total_properties` and
`discovery_pagination` on discovery responses. Existing always-present fields (`blueprint_path`,
`class`, `parent_class`, `properties`, `count`) are unchanged; `count` remains the number of properties
returned (page size in metadata mode).

### Compile-fix note (UE 5.8)

`FJsonObject` storage is keyed by `UE::FSharedString` in UE 5.8, so a direct
`Params->Values.Find(FString(Field))` lookup does not convert and failed the first controller build
(`CortexBPClassDefaultsOps.cpp:414,443`). The fix uses the public accessor
`const TSharedPtr<FJsonValue> Found = Params ? Params->TryGetField(Field) : nullptr;`, which is the
idiom already used across the plugin (`CortexBPRemoveGraphOps.cpp`, `CortexCommandRouter.cpp`,
`CortexData/*`) and works with the `const TCHAR*` field names. This was the only compile fix; no other
code or test change was needed before GREEN.

## Controller-observed RED (before implementation)

Staged consumer-visible regressions failed against the previous production behaviour:

| Filter | Result | Failure signature |
|---|---|---|
| `Cortex.Blueprint.TokenRead+` | 5/5 FAIL | metadata reads extracted `value`/`members` and omitted `values_included`/`total_properties`/`discovery_pagination`; `include_values`/`include_members` were not opt-in; invalid bool/limit selectors were not refused; discovery was not paged. |
| `Cortex.Blueprint.Discovery+` | 2 PASS / 4 FAIL | the two new tests (metadata-only defaults, invalid discovery controls) failed; the migrated explicit-value tests already passed. |
| `MCP/tests/test_class_defaults.py` | 1 FAIL | the tool did not accept/forward `include_values`/`include_members`/`discovery_offset`/`discovery_limit`. |

Logs: `Saved/TestLogs/…003906.log` (TokenRead RED), `…003936.log` (Discovery RED).

## Controller GREEN

Serial covering runs (one runner at a time), after the compile fix:

| Filter / suite | Result | Log |
|---|---|---|
| `Cortex.Blueprint.TokenRead+` | 5/5 PASS | `Saved/TestLogs/…010239.log` |
| `Cortex.Blueprint.Discovery+` | 6/6 PASS | `Saved/TestLogs/…010311.log` |
| `Cortex.Blueprint.ClassDefaults.Get+` | 8/8 PASS | `Saved/TestLogs/…010341.log` |
| `MCP/tests/test_class_defaults.py` + `MCP/tests/test_reflect_tools.py` | 33 passed | controller pytest run |

Raw `Warning|Ensure|Fatal|Assertion` severity scan of the run logs: clean (no unexpected diagnostics).
The staged fixtures create no disk assets, call no global GC and retire only their own dedicated
package, so no package/directory-watcher warnings are expected or observed.

## Post-review corrections (review round 1)

Three controller-source-confirmed review findings were fixed minimally after the GREEN above; the
covering filters must be re-observed (controller-owned) against this revision:

1. **Present non-array `properties` degraded to an unfiltered read.** `ReadOptionalPropertySelector`
   (shared by all three reads) now checks presence first and refuses a present non-array selector with
   `INVALID_FIELD` (`FJsonObject::HasField` + `TryGetArrayField`); only absent means "no selector".
2. **Controls were validated after asset loading.** Both entrypoints now parse the controls and the
   selector immediately after the params/path presence check and before `LoadBlueprint`/`GetBlueprintCDO`,
   so a malformed flag/limit/selector refuses with `INVALID_FIELD` instead of compiling the Blueprint or
   reporting `BLUEPRINT_NOT_FOUND`/`COMPILE_FAILED`.
3. **The MCP tool broke positional callers.** `get_class_defaults` restored the original
   `(asset_path, properties, blueprint_path)` positional order and placed the four new controls after
   `blueprint_path`; keyword use is unaffected.

Targeted regressions added (all staged before this fix round):

* Both `InvalidReadControlsRefuse` tests gained present string/object/null `properties` cases (including
  `include_values=true` on one) requiring `INVALID_FIELD`.
* Both tests gained a compile-less Blueprint fixture (dedicated package, `NewObject<UBlueprint>` with the
  same public flags as `CreateBlueprint` but no generated class) plus a malformed control: the read must
  refuse with `INVALID_FIELD` and must leave `GeneratedClass` null. The `get_class_defaults` case also
  asserts the path resolves through `FCortexBPAssetOps::LoadBlueprint` **without compiling**, so the
  fixture is a genuinely resolvable Blueprint that lacks a generated class.
* Both tests gained a nonexistent-path + malformed-control case refusing with `INVALID_FIELD` (never
  `BLUEPRINT_NOT_FOUND`), proving the refusal precedes the lookup.
* `MCP/tests/test_class_defaults.py` gained the legacy three-positional call asserting normal asset-path
  forwarding.

## Registered smoke (controller-owned)

`blueprint.get_class_defaults` / `list_settable_defaults` / `list_inherited_properties` against a
disposable Blueprint: default read returns metadata with `values_included=false` and a complete
100-limit discovery page; `include_values=true` recovers the effective values (including `false`);
`include_members=true` adds member path/type metadata with no member values until `include_values=true`;
`discovery_limit=7` pages repeatedly without skipping or duplicating a name; then only that Blueprint is
retired. Controller-owned, not claimed here.

## Task 17 handoff (not done here)

* `MCP/src/cortex_mcp/_fallback_generated.py` and `MCP/tests/fixtures/capabilities_cache_full.json`
  still describe the three read commands without `include_values`/`include_members`/`discovery_offset`/
  `discovery_limit`; regenerate after a live capabilities read of the updated native schema. The
  Python fallback/capabilities pair stays internally consistent meanwhile, so no current Python test
  fails.
* Root `docs/systems/cortex-blueprint.md` and toolkit
  `resources/{blueprint-development,blueprint-patterns,mcp-tool-reference,bp-migration-verifier}.md`
  still describe `get_class_defaults` discovery as returning values; Task 17 owns the wording sync
  (metadata discovery is now the default, values are opt-in via `properties`/`include_values`).
* `MCP/src/cortex_mcp/response.py` already maps the three commands to the
  `properties` / `include_values=false` / `discovery_limit` guidance (Task 1), which matches the new
  contract and needs no change.
* No other consumer asserts discovery values: no Python tool calls `get_class_defaults` internally,
  `blueprint.list_settable_defaults`/`list_inherited_properties` are cached reads that do not read
  values, and the composite/toolkit callers are documentation-only. `MCP/tests/fixtures/
  mcp_tool_signature_snapshot.json` is not referenced by any test.
* `MCP/tests/test_e2e.py` and `MCP/tests/test_mcp_scenarios.py` read values only with explicit
  `properties`, so they remain valid under the metadata-only default.
