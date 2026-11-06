# Cross-domain token-efficiency implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended for independent domain slices) or superpowers:executing-plans (for the shared Python boundary and coupled lifecycle work) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the reviewed cross-domain token-efficiency contracts without losing targeting, effective values, completeness or mutation outcomes.
**Architecture:** Keep extraction/transactions in their existing native domain owners. One Python public registration adapter controls result representation and startup visibility; explicit native/cached pagination policies replace heuristic ownership. Update related toolkit guidance and offline/context entry points as scoped consumers.
**Tech Stack:** UE 5.6+ C++ (validation on Windows UE 5.8), Python>=3.10, mcp>=1.26.0, Pydantic/FastMCP, existing TCP/stdio transport and pytest/Unreal automation.
**Spec:** [reviewed design](2026-10-13-token-efficiency-design.md); [evidence audit](2026-10-13-token-efficiency-audit.md).
**Status:** independently reviewed; ready for approved execution; no product code implemented. Plugin base `9f899fafeb18666cc8204f7446120d411491861f`; toolkit base `fb5f959c3e42a14296c802b904b355f99785d174`. Review findings and clearance are recorded in [review record](2026-10-13-token-efficiency-review.md).

## Global constraints

- This invocation authorizes documentation only; do not execute these product changes, dependency updates or tests before artifact/execution approval. Publication is separately authorized.
- Python remains >=3.10; adopt mcp>=1.26.0; UE supported floor remains5.6. Current configured engine is `D:/UnrealEngine/UE_5.8`.
- Every shell command uses rtk. Normal owning-repository branches only; no worktrees/reset/clean/stash/force or unrelated process shutdown. Preserve parent dirt/gitlinks and generated user snapshots.
- UObject access/mutation is Game Thread-only. No cross-domain module coupling, game-specific API or new standalone MCP tool. Existing explicit compile/save/rollback/approval guards stay authoritative.
- Clean cutover of each changed contract: all registered callers, native operation schema, generated fallback, tests and toolkit guidance together; no aliases. Keep unrelated raw-export aliases/contracts unchanged.
- MAX_RESPONSE_CHARS=40000 bounds compact JSON TEXT, not whole MCP wire bytes. No generic truncation of native pages, write outcomes or approval inventories. Hash/input/archive encoders unchanged.
- Defaults: response text; toolset all; discovery metadata without values unless selected; include_members=false; changed_only=false; catalog include_asset_paths=false; component include_properties=false; Reflect standalone summary; Gen include_prompt=false; class-default echo=diff.
- All substantive behavior is TDD: failing consumer-visible regression, observed RED, minimal implementation, observed GREEN and actual smoke. Native RED and GREEN each require compiling the changed test/code first. No production scaffolds/no-op fallbacks. Controller runs acceptance independently; agents skip build/lint/tests/formatters mid-flight.
- Use existing fixture conventions. New Blueprint fixtures own dedicated packages: retire BP/generated/skeleton classes/CDOs but do not force-null UField metadata; never apply whole-package retirement to GetTransientPackage(). Guard LoadObject by package existence; do not call global GC as a cleanup shortcut. Native suites have zero unexpected warnings/errors.
- Docs are part of each completed behavior task. Complete native interface/schema changes before generating live capabilities/fallback. Remove only task-owned smoke scripts/assets after proof, preserving reports.

## Review focus

1. Native limit plus offset and oversized rows: returned data must never retain a continuation that skips omitted records (Tasks3,4,11).
2. Parent baseline versus subclass fields, zero/false/null and FText: delta must not access parent-sized memory or hide a real clear (Tasks6,7,13).
3. Last/only QA step, error beyond first page, ring eviction/new logs during continuation: detect once or report incomplete coverage (Task11).
4. Tree depth/node cut plus asset-owned orphan bindings: an incomplete branch is not a leaf or an empty binding inventory (Task10).
5. Text/structured/opaque/async outputs and Unicode metrics under approval budgets: presentation must preserve error/outcome and strict invocation (Tasks1,2,17).

## Task ownership and dependency map

Paths below are plugin-relative unless prefixed `cortex-toolkit/` or workspace `docs/`. The controller owns shared response.py/routers.py/server.py/tool_output.py and metadata integration. A domain worker edits only its assigned Operations/handler/tests; communicate registry needs to controller rather than sharing edits.

| Task | Scope / TE | Depends on | Exclusive primary edit ownership |
|---|---|---|---|
| 1 | Public output/metrics TE01,17 | approval | MCP output adapter/formatter/graph boundary/server/tcp metrics |
| 2 | Discovery/toolsets TE02 | 1 | capabilities, registration visibility, public description wrappers |
| 3 | Pagination TE03 | 1 | routers, pagination cache, native-page formatter |
| 4 | Asset filter/page contracts TE04 | 3 | Data asset search/list selector and Data handler |
| 5 | Selected DataAsset/material values TE05 | 4 (shared DataAssetOps ownership) | DataAsset get read; Material graph/handler |
| 6 | Defaults discovery/member/index TE05,07 | 1,3 | Blueprint defaults read/helper/handler/token tests |
| 7 | Inherited deltas TE06 | 6 | same Blueprint defaults file + Core serializer |
| 8 | Catalog/capability views TE08 | 2,4 | Data catalog/handler; controller core router projection |
| 9 | Level component inventory TE09 | 3 | Level component ops/handler/tests |
| 10 | Bounded UMG tree TE10 | 1,3 | UMG tree ops/handler/tests |
| 11 | Log pages and finite QA coverage TE12 | 1,3 | Editor capture/utility/handler + QA Python consumers; controller registry update |
| 12 | Reflection lean views TE11 | 3 | Reflect ops/handler and Python Reflect detail/context |
| 13 | Job prompt inventory TE13 | 2,3 | Gen handler/job tests; no provider/network changes |
| 14 | Class-default echo TE14 | 6,7 | same Blueprint defaults write/test/handler |
| 15 | Linked offline catalog TE15 | 1 | schema generator/tests |
| 16 | Same-turn frontend context TE16 | 1 | InputArea/Codex provider/tests |
| 17 | Synchronize/integrate/prove all | 1-16 | controller: fallback/docs/toolkit/acceptance evidence |

Tasks9/10/12/13/15/16 can be independent investigation/edit slices after their prerequisites, with no shared handler files. Shared Python ownership remains controller-inline; serialize native build/test/editor use. Tasks4/5 and6/7/14 are sequential because they share authoritative owner files. Never delegate a mechanical registry addition or docs-only trim alone.

## Commands and evidence conventions

Run Python commands from `Plugins/UnrealCortex/MCP`:
```bash
rtk proxy uv run pytest tests/test_response.py -v
rtk proxy uv run pytest tests/ -m "not e2e and not scenario and not stress" -v
```

For C++ tasks run this build from workspace root after RED tests are written and again after the minimal fix. Shell scripts through the Eval tool if executed by this harness:
```bash
rtk bash -lc 'export DOTNET_ROOT="D:/UnrealEngine/UE_5.8/Engine/Binaries/ThirdParty/DotNet/10.0/win-x64" DOTNET_MULTILEVEL_LOOKUP=0 DOTNET_ROLL_FORWARD=LatestMajor; rtk proxy "D:/UnrealEngine/UE_5.8/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe" CortexSandboxEditor Win64 Development -Project="D:/UnrealProjects/CortexSandbox/CortexSandbox.uproject" -WaitMutex -FromMsBuild -NoHotReloadFromIDE -NoLiveCoding'
```

Native runner template, root cwd; substitute the EXACT task filter named below, one invocation at a time:
```bash
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex.Data+' -Timeout 600 }"
```
Do not run a manual task Editor concurrently with RunTests.ps1 against shared project assets. Status is `RunTests.ps1 -ReportOnly`; never poll background jobs in the assistant. Check raw severity-tagged diagnostics, not only summary counts.

Use LSP references before exported symbol/signature changes, and project-rooted clangd before native navigation. Adding sources requires local compilation-database regeneration. Do not copy another checkout's database. Scoped test filters and APIs below are planned additions, not claims they already exist.

---

## Task 1 — One public output representation and correct size metrics

**Files:** Create `MCP/src/cortex_mcp/tool_output.py`; modify `response.py`, `graph_patch_boundary.py`, `server.py`, `tcp_client.py`, `MCP/pyproject.toml`, lockfile; add `MCP/tests/test_tool_output.py`; modify behavioral tests in test_response.py and graph/UMG response-boundary files only where encoding affects real budget cases. Check native frontend stream parsing through existing CortexStreamEventParseTest.cpp/CortexCodexAppServerProtocolTest.cpp; do not restyle widgets.
**Interfaces:** Produce `compact_json(value)->str`; `ToolOutputRegistrar(mcp_server, connection, response_mode="text", allowed_tools=None, metrics=False).tool(name=None, description=None, **kwargs)`; `UEConnection.record_response_size(tool_name, qualified_command, json_chars, utf8_bytes)`; existing get_call_metrics adds aggregate sizes. allowed_tools=None exposes all current registrations; an explicit frozenset is a real registration allowlist, not an unimplemented toolset name. Preserve existing strict-router `_tool_manager` argument-model customization. The new adapter is a small registration facade, not a general plugin framework.

- [ ] Write RED tests using actual FastMCP/stdio registration of shipped tools, not MockMCP forwarding. Use a temporary project schema for core_cmd(schema_status), Reflect cache status for standalone, and real disposable-asset composite smoke later. Assert parsed data/Unicode/null/large integer and failure semantics, not description wording. Encoding assertion in the stdio test:
```python
result = await session.call_tool("core_cmd", {"command": "schema_status", "params": {}})
text = next(block.text for block in result.content if block.type == "text")
assert result.structuredContent is None  # default text-only
assert "\n" not in text
assert json.loads(text)["exists"] is True
```
Add opaque error preservation, async optional Gen registration without a fake provider mutation, metrics-on Unicode chars versus bytes and error/result counts. Dedicated graph previews must retain the entire GUID set or refuse; applied authoring must retain outcome. Keep genuine existing boundary tests; delete source/wording-only tests rather than re-pin incidental formatting.

The test file defines this real stdio-client helper and creates a temporary project `.uproject`, `Saved/Cortex/capabilities-cache.json` and `.cortex/schema` before invoking schema_status. Set metrics mode only through the future public startup setting; no fake TCP replies are needed for the local schema read.
```python
import asyncio
import json
import os
from contextlib import asynccontextmanager
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

@asynccontextmanager
async def open_test_client(project_dir, response_mode="text", toolset="all", metrics=False):
    child_env = dict(os.environ, CORTEX_PROJECT_DIR=str(project_dir),
                     CORTEX_RESPONSE_MODE=response_mode, CORTEX_TOOLSET=toolset,
                     CORTEX_RESPONSE_METRICS="1" if metrics else "0")
    params = StdioServerParameters(command="uv",
        args=["run", "python", "-m", "cortex_mcp.server"], env=child_env)
    async with stdio_client(params) as (reader, writer):
        async with ClientSession(reader, writer) as session:
            await session.initialize()
            yield session
```
Use an actual temporary directory name containing Unicode for the schema path and assert emitted `_meta.cortex` bytes/chars differ correctly. Call-site tests enter `async with open_test_client(...) as session`; pytest sync cases may run their async scenario with asyncio.run. Keep cwd at MCP so uv resolves the actual package.
- [ ] Run `rtk proxy uv run pytest tests/test_tool_output.py tests/test_response.py tests/test_graph_prune_response_boundary.py tests/test_graph_rewire_response_boundary.py tests/test_graph_retirement_response_boundary.py -v`; record the actual nested-string/pretty budget failure or missing API RED.
- [ ] Implement compact output only; the minimum normal encoder is:
```python
def compact_json(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)

# The registered wrapper builds this, preserving the callable's input signature.
text = compact_json(data)
result = CallToolResult(content=[TextContent(type="text", text=text)],
                        structuredContent=data if response_mode == "structured" else None)
```
Use functools.wraps and separate sync/async wrappers; structured_output=False prevents SDK output-schema/string wrapping. Preserve explicit CallToolResult/content; non-JSON strings stay opaque text, never empty dictionaries. Metrics go to `_meta.cortex` and count the emitted body, excluding metadata, once per public return. Route all registrations through the adapter, including captured legacy composite implementations, Reflect and import queues; leave capture-only helpers internal. Update all response budget encoders to compact_json, not hash/archive encoders. Adopt SDK floor and validate startup settings.
- [ ] Re-run the named tests, then actual stdio text/structured/metrics probes. Check sync/async, strict extra-field refusal, native error triplets, exact approved sets and opaque failure display. GREEN does not claim a native compile/runtime pass.
- [ ] Update python-mcp/system/verification docs for this task and its tested commit identity. Commit only if execution authority includes commits; otherwise leave the scoped verified work local.

## Task 2 — Short discovery and optional startup toolsets

**Files:** capabilities.py; tool_output.py/server.py; registered composite wrappers under `MCP/src/cortex_mcp/tools/composites/{blueprint,material,widget,level,statetree,scenario,gen}.py`; standalone wrappers; `MCP/src/tools/reflect/*.py` descriptions; tests/test_capabilities.py and test_tool_output.py. Do not mutate full fallback operation metadata to slim descriptions.
**Interfaces:** Produce `resolve_toolset(name, registered_domains)->frozenset[str]|None` and names-only description rendering; all returns None (no filter), named presets return the exact design §2.2 allowlist passed as ToolOutputRegistrar.allowed_tools. Toolset visibility is not execution policy.

- [ ] RED: actual tools/list for each preset, fallback without capabilities cache, optional domains absent/present, invalid preset startup; strict command envelope and live schema discovery remain executable. Preserve self-sufficient blueprint_compose update contract in its concise description. One actual UI style read must work with ui despite needing Blueprint/Data routers.
```python
assert {t.name for t in (await session.list_tools()).tools} == expected_names
# Then exercise the exposed routing surface, rather than merely snapshotting names.
await session.call_tool("core_cmd", {"command": "schema_status", "params": {}})
```
The session helper returns actual ClientSession and runs inside its context manager; expected_names is the design's explicit membership with optional availability applied. Do not use a copied wiring-only schema snapshot as new coverage.
- [ ] Run `rtk proxy uv run pytest tests/test_capabilities.py tests/test_tool_output.py -v`; expect current signatures/full preset or invalid configuration behavior to fail.
- [ ] Generate descriptions from the same metadata command-name list, keeping `_COMPOSITE_HINTS` safety/trust guidance and canonical usage. Do not create a second fallback command list. Example rendering core:
```python
names = ", ".join(command["name"] for command in commands)
body = f"Route through {domain}_cmd(command, params). Commands: {names}. " \
       "Fetch parameters with core.get_operation_schema before unfamiliar calls."
```
Retain declaration examples for Python composites; native schema cannot replace them. Resolve allowed tools once at startup; registration decorators for excluded tools return the unregistered callable without emitting a public tool. Preserve optional-domain registration gate. Do not delete Reflect standalone wrappers or conflate toolsets with UMGAuthoring.
- [ ] GREEN and stdio presets; record definition chars and one valid/refusal operation in each preset. Cached/fallback descriptions need no live editor; actual live operation schema proof occurs Task17.
- [ ] Update startup env/client restart and tool reference docs; task-scoped commit only with authority.

## Task 3 — Explicit native/cached pagination, no cursor skipping

**Files:** routers.py, pagination.py, response.py; existing tests/test_routers.py/test_response.py; add behavioral `MCP/tests/test_pagination_contract.py` only for consumers not already protected by those tests.
**Interfaces:** Produce NATIVE_LIMIT_COMMANDS for actual native owners; CACHED_PAGE_ARRAYS mapping qualified read command to primary array; format_native_page(data, qualified_command, params)->str complete-or-refuse. Extend format_response with qualified_command and request_params keyword context while retaining existing short command/detail parameters for dedicated animation/binding policies. PaginationCache entries retain command/array/template provenance; emit cursor only after final bounded page length. Task11 adds editor.get_recent_logs when its native API lands; no unsupported future entry advertised early.

- [ ] RED: native query limit60 offset7 on a table with >=80 rows; list_actor limit>100; native Gen0/editor CVar500; no Python cursor added to native pages. Cache-owned list of7 items requested limit2 must yield four contiguous pages. An oversized cached page must not skip IDs on the next cursor; route-mismatched/expired cursors refuse; native oversize must refuse without advancing continuation. Dedicated guarded branches still reject pagination before mutation.
- [ ] Run `rtk proxy uv run pytest tests/test_pagination_contract.py tests/test_routers.py tests/test_response.py -v` for Python behavioral cases, then the existing live Data/Level E2E families after controller creates the fixture. Record native versus Python failure layer; never accept a mock echo as proof a native limit was honored.
- [ ] Replace the current ordering/guessing:
```python
if (domain, command) in NATIVE_LIMIT_COMMANDS:
    response = connection.send_command(qualified, route_params)
    return format_native_page(response["data"], qualified, route_params)
# Only after native ownership, validate 1..200 for cache-owned reads.
array_key = CACHED_PAGE_ARRAYS.get(qualified)
```
Keep UMG/graph/property-binding branches first. Mapping: data lists datatables/data_assets/curve_tables/string_tables/tags; level.find_actors matches, level class lists classes; graph.list_graphs graphs/search_nodes results/list_nodes and handler/function-call discovery nodes; blueprint.list blueprints/list_scs_components components; material.list_materials materials/list_instances instances/list_collections collections/list_nodes nodes. Verify these existing array identities from owner source before adding a row; no speculative entries for mixed objects/trees. Native set: Data query/search_content/search_assets, Level list_actors, Reflect five limit consumers, StateTree list_assets, Editor list_cvars, Animation list_assets, Gen list_jobs.

For cached pages, find the largest prefix fitting compact text INCLUDING pagination metadata; compute returned/next_cursor from that prefix, store the full snapshot and original total. If even one item cannot fit, refuse with no advancing cursor. Validate cursor command/provenance. For native pages, refuse full oversized page with current selector/window metadata and a smaller-limit/fields suggestion; never use generic array shortening.

Controller-owned TE07 dispatch is part of this task, not deferred to object callers. Pass exact qualified_command/request_params on normal reads and native errors. Explicit object/tree policies precede generic array truncation and follow protected binding/mutation branches: data.get_data_asset -> fields; material.get_node -> properties; blueprint get/list defaults -> properties, include_values=false or discovery_limit; umg.get_tree -> depth/max_nodes/names_only or get_widget; core capability facade -> detail=names/per-operation schema; data catalog -> sections/include_asset_paths=false; Reflect detail/context -> detail=summary/properties. These reads are complete-or-refuse: preserve selector/type/partial/issue or binding context, reader_complete=false on refusal, and never advertise limit when that read cannot consume it. Dedicated authoring outcome and orphan-binding branches remain authoritative. Add RED public-route oversize cases for DataAsset, material-node and ordinary tree reads against task-owned large fixtures; the refusal must name the applicable selector and must not pretend values/tree are complete. The same cases run GREEN after Task5/10 selectors are implemented.
- [ ] GREEN; actual native/public-route page traces reconstruct exact requested populations. Preserve dedicated graph/UMG refusal/outcome proof; do not rerun unrelated native suites now.
- [ ] Update pagination docs, task evidence and scoped commit only with authority.

## Task 4 — Fix Data asset selectors and add truthful search projection/pages

**Files:** Source/CortexData/Private/Operations/CortexAssetSearchOps.cpp, CortexDataAssetOps.cpp (list only), CortexDataCommandHandler.cpp; existing CortexDataAssetSearchTest.cpp and CortexDataAssetCrudTest.cpp; MCP test_e2e.py behavior, not forwarding snapshots.
**Interfaces:** search_assets class_names/path_prefixes arrays, fields/count_only/offset and native limit; list_data_assets class_name; response assets/count/total_matches plus offset/limit/next_offset/is_complete. No new MCP tool or alias.

- [ ] In dedicated package folders `/Game/Temp/CortexTokenSearch_<GUID>/Inside` and sibling `InsideExtra`, create two test DataAssets and a different-class asset, register/save them using existing Data asset fixture patterns. RED expects only canonical Inside/class matches, exact filtered count independent of limit, a deterministic next page and fields=[path] excluding unrelated metadata. Invalid arrays/paths refuse instead of scanning all assets. Use a native regression body:
```cpp
Params->SetArrayField(TEXT("path_prefixes"), {
    MakeShared<FJsonValueString>(Root + TEXT("/Inside"))});
Params->SetArrayField(TEXT("class_names"), {
    MakeShared<FJsonValueString>(TEXT("CortexTestDataAsset"))});
Params->SetNumberField(TEXT("limit"), 1);
const FCortexCommandResult Result = Router.Execute(TEXT("data.search_assets"), Params);
TestTrue(TEXT("search succeeds"), Result.bSuccess);
double TotalMatches = -1;
TestTrue(TEXT("filtered total exists"), Result.Data->TryGetNumberField(TEXT("total_matches"), TotalMatches));
TestEqual(TEXT("post-filter total"), TotalMatches, 2.0);
```
`Root`/Router are owned fixtures built at the start of the RunTest; no real-project asset names. Add class_name list regression for current silent selector drift.
- [ ] Build; run `Cortex.Data.SearchAssets+` and affected `Cortex.Data.DataAsset+` tests serially; expect unrelated folder/class records or absent total metadata RED.
- [ ] Implement FARFilter PackagePaths recursion from canonical folder arrays, OR class names with exact native class semantics and AND category intersection. Validate array element strings before registry fetch, canonicalize/reject malformed prefixes. Filter all candidates, sort object path, compute total_matches, then offset/limit; count_only skips row construction. Projection whitelist path/name/class_name/package_path; preserve canonical path. Migrate list class_filter to class_name and all registered/toolkit consumers in Task17; do not touch raw-export compatibility names.
- [ ] Build/GREEN; actual data_cmd read/count/page smoke against the saved fixture must match native results with correct limit. Inspect absence of outside siblings and total count; don't claim via server success alone.
- [ ] Update Data design/system docs and owned fixtures/evidence; scoped commit only with authority.

## Task 5 — Select DataAsset and material-node values before expansion

**Files:** CortexDataAssetOps.cpp GetDataAsset; CortexDataCommandHandler.cpp; Source/CortexMaterial/Private/Operations/CortexMaterialGraphOps.cpp; CortexMaterialCommandHandler.cpp; CortexDataAssetCrudTest.cpp/CortexMaterialGraphTest.cpp; existing serializer deep-read tests.
**Interfaces:** Data get_data_asset fields:string[]; Material get_node properties:string[]; absence/empty retains full read. Reuse Core ResolvePropertyPath and PropertyToJsonDeep; preserve admission, type and partial/issues.

- [ ] RED: test asset includes a selected zero/false/null/localized text field and an unrelated large nested value; selected read exposes only requested property paths and its actual diagnostics. Material expression select existing editable property while preserving node_id/collection_path/parameter_name. Unknown paths refuse, not full fallback.
```cpp
Params->SetArrayField(TEXT("fields"), {
    MakeShared<FJsonValueString>(TEXT("TestNumber"))});
const FCortexCommandResult Selected = FCortexDataAssetOps::GetDataAsset(Params);
TestEqual(TEXT("exact selected field count"),
    Selected.Data->GetObjectField(TEXT("properties"))->Values.Num(), 1);
```
`Tests/CortexTestDataAsset.h:50-63` declares TestProperty, TestNumber=0, Nested.Visible/Internal, NestedArray and UnsupportedExportInterface. Use TestNumber for explicit zero, Nested.Visible for leaf selection and the actual UnsupportedExportInterface for diagnostics. Add false/null/localized-text fixture UPROPERTYs only to the test class when needed; no production class/game logic. For diagnostics exercise the real unsupported/depth-limited selected field, not mocked issues.
- [ ] Build/run `Cortex.Data.DataAsset+` and `Cortex.Material.Graph+`, expect ignored selection RED.
- [ ] Resolve each selected path, validate root exposure, serialize only its ValuePtr with existing deep policy. Selected Data properties become a path-keyed object consistent with class-default selected reads; no global recursive admission-filter by top-level name. Aggregate issues/bPartial for selected values. Material loops restrict serialization to selected editable fields; preserve special selectors and full fallback.
- [ ] Build/GREEN and actual selected/full public reads prove same requested values and smaller bytes, including text source metadata and explicit clears.
- [ ] Update schemas/docs and task evidence; scoped commit only with authority.

## Task 6 — Bounded metadata discovery and single struct representation

**Files:** CortexBPClassDefaultsOps.cpp/h (read helpers), CortexBPCommandHandler.cpp; add `Source/CortexBlueprint/Private/Tests/CortexBPTokenReadTest.cpp`; revise consumer-visible portions of CortexBPClassDefaultsTest.cpp/CortexBPDiscoveryOpsTest.cpp and MCP/test_class_defaults.py. Native source addition requires UBT/local LSP regeneration, not manual source-list editing.
**Interfaces:** Existing three defaults reads gain properties/include_values/include_members/discovery_offset/discovery_limit; metadata label values_included; discovery_pagination. `BuildDiscoveryPropertyInfo` receives explicit value/member inclusion while preserving discovery-only accepted formats. Get's selected property paths preserve effective values.

- [ ] New test-local `CreateTokenReadBlueprint(parent_class, unique_path)` calls CreatePackage then FKismetEditorUtilities::CreateBlueprint/CompileBlueprint in a dedicated package. Use real Actor inherited bReplicates and existing test lift struct properties. Retire ONLY its dedicated package using CortexBPRemoveGraphPersistenceTest.cpp:188-206 pattern; never use this retirement on existing transient discovery fixtures. Add RAII cleanup after each test; no global GC or missing-package LoadObject. RED requires unfiltered metadata-only response with no value/member extraction, explicit values recover bReplicates=false, properties leaf selects exact effective value, include_members opt-in, discovery pages neither duplicate nor skip names, invalid selectors/booleans refuse.
```cpp
const FCortexCommandResult Meta = Handler.Execute(TEXT("get_class_defaults"), Params);
bool bValuesIncluded = true;
TestTrue(TEXT("discovery declares value inclusion"),
    Meta.Data->TryGetBoolField(TEXT("values_included"), bValuesIncluded));
TestFalse(TEXT("metadata does not claim values"), bValuesIncluded);
Params->SetArrayField(TEXT("properties"), {
    MakeShared<FJsonValueString>(TEXT("bReplicates"))});
const FCortexCommandResult Value = Handler.Execute(TEXT("get_class_defaults"), Params);
TestFalse(TEXT("false is a value, not omission"), Value.Data->GetObjectField(TEXT("properties"))
    ->GetObjectField(TEXT("bReplicates"))->GetBoolField(TEXT("value")));
```
Observe metadata extraction through a real heavy/unsupported selected property (no serializer invocation for metadata); do not add source-text/implementation-call-count assertions.
- [ ] Build/run `Cortex.Blueprint.TokenRead+` and affected `Cortex.Blueprint.Discovery+`; expect current full-value/default-member behavior RED.
- [ ] Parse flags/types first. For Get default include_values = nonempty properties unless explicitly supplied; discovery defaults false. Page admitted property metadata with discovery_limit1..200/default100; explicit selected value reads are not silently paged. Add members only when requested, and member values only if include_values. Preserve FText metadata, categories and accepted formats. Restrict settable/inherited roots before leaf resolution. On overflow return bounded name inventory/refusal with correct properties/discovery_limit/include_values guidance, without pretending sizes are known before extraction.
- [ ] Build/GREEN plus actual blueprint_cmd unfiltered/selective/full/members/page smoke. Ensure known composite/default consumers now request values explicitly; finish that migration before task acceptance.
- [ ] Update Blueprint schema and task docs/evidence; scoped commit only with authority.

## Task 7 — Ownership-safe inherited default delta

**Files:** CortexBPClassDefaultsOps.cpp; CortexBPCommandHandler.cpp; Source/CortexCore/Public/CortexSerializer.h and Private/CortexSerializer.cpp; CortexBPTokenReadTest.cpp and existing Core serializer behavioral tests. No exposing old private whole-object helpers.
**Interfaces:** New public `FCortexSerializer::PropertyDifferenceToJson(Property, ValuePtr, BaselineValuePtr, Policy, FieldPath)->FCortexPropertySerializationResult`; Blueprint changed_only=false; value_mode=inherited_delta/baseline_class/effective_values_complete=false/omitted_equal_properties. Only Blueprint owns selection of parent baseline.

- [ ] RED consumer regression: parent bReplicates=true, child bReplicates=false plus newly declared boolean OwnOnly=false; delta must retain both false values, omit equal inherited field with explicit omitted metadata, and not read child fields from parent memory. Include zero/null/FText override and same-value selected property; label delta. Use native fields through genuine parent/child generated classes, not raw struct buffers pretending to be UObject CDOs.
```cpp
Params->SetBoolField(TEXT("changed_only"), true);
Params->SetBoolField(TEXT("include_values"), true);
const FCortexCommandResult Delta = Handler.Execute(TEXT("get_class_defaults"), Params);
FString ValueMode;
TestTrue(TEXT("delta declares its semantics"), Delta.Data->TryGetStringField(TEXT("value_mode"), ValueMode));
TestEqual(TEXT("delta label"), ValueMode, FString(TEXT("inherited_delta")));
TestTrue(TEXT("subclass-owned false is retained"),
    Delta.Data->GetObjectField(TEXT("properties"))->HasField(TEXT("OwnOnly")));
```
OwnOnly is added to the test BP using FBlueprintEditorUtils::AddMemberVariable with PC_Boolean; compile before read, clean up the dedicated fixture after.
- [ ] Build/run `Cortex.Blueprint.TokenRead+` and `Cortex.Core.Serializer+`; these are the existing Core serializer test namespace and planned new Blueprint family. Expect missing/unsafe delta semantics RED.
- [ ] Compute baseline per admitted root only when parent class owns the same compatible FProperty; verify field ownership/type and offset within parent instance layout before obtaining pointer. Subclass roots serialize full effective values. Core recursion: Identical => no value; struct => recursively compare members of SAME struct type; other values including arrays/maps/sets/objects => atomic existing deep serialization. Depth limit returns structured issue+bPartial, never `<changed>` as a complete value. Blueprint records equal omissions and baseline; explicit include_values=false with changed_only refuses. Do not call NonDefaultPropertiesToJson(CDO) or private helpers from Blueprint.
- [ ] Build/GREEN and actual parent/child delta/full reads, followed by subsequent compile/GC lifecycle smoke; effective values remain unchanged and no warnings/ensures.
- [ ] Update Core/Blueprint system docs for new helper and semantic view; scoped commit only with authority.

## Task 8 — Lean Data catalog and agent capability inventory

**Files:** CortexDataTableOps.cpp GetDataCatalog; CortexDataCommandHandler.cpp; controller routers.py core facades; CortexDataCatalogTest.cpp; test_routers.py/test_capabilities.py behavior.
**Interfaces:** sections list/all default and include_asset_paths=false for catalog; core_cmd get_capabilities detail=names|full Python-owned projection. Full native handshake/cache untouched.

- [ ] RED: large class catalog exposes class count/example but no full asset_paths by default; selected sections don't build unrelated tables/assets; explicit true recovers exact full paths. Agent names-only capability response preserves domains/command names; a subsequent live operation schema still has params and cache remains full.
```cpp
Params->SetBoolField(TEXT("include_asset_paths"), true);
const FCortexCommandResult Full = Router.Execute(TEXT("data.get_data_catalog"), Params);
TestEqual(TEXT("path inventory matches class count"),
    ClassEntry->GetArrayField(TEXT("asset_paths")).Num(), ClassEntry->GetIntegerField(TEXT("count")));
```
ClassEntry is obtained from Full.data_asset_classes for the fixture's exact class; no assumption about the entire project's class population.
- [ ] Build/run `Cortex.Data.Catalog+`; Python `rtk proxy uv run pytest tests/test_routers.py tests/test_capabilities.py -v`; observe current deep/default view RED.
- [ ] Parse/validate sections before per-section registry/load work; build only selected sections. Skip asset-path arrays unless requested; cache params include selection. Core facade validates/removes its Python detail before native call and projects command records after receiving full authoritative data; never changes UEConnection capability handshake or writes projected data to Saved cache.
- [ ] GREEN and actual core/data facades selective/full catalog plus names/full capabilities/live operation-schema read; record exact byte reduction.
- [ ] Update catalog/capability docs and task evidence; scoped commit only with authority.

## Task 9 — Metadata-only Level component lists

**Files:** CortexLevelComponentOps.cpp SerializeComponent/ListComponents; CortexLevelCommandHandler.cpp; CortexLevelComponentTest.cpp; affected MCP level behavior tests.
**Interfaces:** include_properties=false default; true retains up-to20 existing editable values plus total_property_count/returned_property_count/properties_complete. Targeted get_component_property remains full retrieval.

- [ ] RED in existing CreateLevelRouterComponent/SpawnActor fixture: same root StaticMeshComponent identity/class/is_root/creation_method returned without properties by default; include_properties=true exposes editable fields and truthfully labels cap; targeted get recovers an omitted property. No fixture world destruction/global GC.
```cpp
Params->SetBoolField(TEXT("include_properties"), true);
const auto Detail = Router.Execute(TEXT("level.list_components"), Params);
TestTrue(TEXT("detail declares property completeness"),
    Detail.Data->GetArrayField(TEXT("components"))[0]->AsObject()->HasField(TEXT("properties_complete")));
```
Add assertions against the fixture's exact root component/selected property, not just nonempty array.
- [ ] Build/run `Cortex.Level.Component+` with rendering runner; observe always-expanded default or missing completeness RED.
- [ ] Thread include_properties into SerializeComponent; avoid reflected value iteration entirely when false. When true count all admitted properties without serializing omitted fields, retain first20 values and declare completeness. Keep existing actor summaries and per-leaf read semantics unchanged.
- [ ] Build/GREEN and actual level_cmd list/targeted read on task-owned actor; observe metadata and property state, then delete only that actor.
- [ ] Update Level docs/evidence; scoped commit only with authority.

## Task 10 — Honest native UMG tree bounds and names projection

**Files:** CortexUMGWidgetTreeOps.cpp/h BuildWidgetTreeJson/GetTree; CortexUMGCommandHandler.cpp; existing UMG widget/tree tests and UMG property-binding tests; add `Source/CortexUMG/Private/Tests/CortexUMGTreeBoundsTest.cpp` if no existing bounded-family file fits.
**Interfaces:** names_only=false; optional depth>=0 (root0), max_nodes>=1; returned_widgets/total_widgets/tree_complete/reasons and cut-node child_count/incomplete; leaves omit empty children. Independent asset-owned binding inspection stays complete-or-refuse.

- [ ] RED: owned root panel -> nested panel -> leaf and sibling; depth0 returns root with correct omitted child count, max_nodes2 returns exactly2 visited widgets and marks the cut (not a leaf), names-only preserves hierarchy/names. A case-only orphan property binding is preserved by include_property_bindings even when tree cut; unsafe oversized combined read explicitly refuses. Full tree recovers identities; properties are unchanged.
```cpp
Params->SetNumberField(TEXT("depth"), 0);
const auto Cut = Router.Execute(TEXT("umg.get_tree"), Params);
bool bTreeComplete = true;
double ChildCount = -1;
TestTrue(TEXT("tree completeness declared"), Cut.Data->TryGetBoolField(TEXT("tree_complete"), bTreeComplete));
TestFalse(TEXT("cut is not complete"), bTreeComplete);
TestTrue(TEXT("root child count declared"),
    Cut.Data->GetObjectField(TEXT("root"))->TryGetNumberField(TEXT("child_count"), ChildCount));
TestTrue(TEXT("root advertises omitted children"), ChildCount > 0);
```
Fixture creation uses existing UMG test helpers/native add_widget operations in a dedicated Widget Blueprint package; exact designer names survive.
- [ ] Build/run `Cortex.UMG+` affected tree/binding families; expect ignored depth/max_nodes or false leaf RED.
- [ ] Add one traversal budget/context passed by reference (depth/root count/remaining nodes); inspect budget BEFORE child JSON allocation. Reuse tree hierarchy, count total without property expansion. Bindings append through existing owner separately; never apply tree cut to property_binding_state. Emit children only for actual retained children; every cut branch has honest omitted counts/complete flag. Update consumers that access children directly to accept actual leaf omission.
- [ ] Build/GREEN and actual umg_cmd shallow/names/full/cut/bindings reads; inspect actual Widget Designer tree remains unchanged.
- [ ] Update UMG docs/evidence; scoped commit only with authority.

## Task 11 — Canonical bounded logs and complete finite QA windows

**Files:** CortexEditorLogCapture.h/cpp, Operations/CortexEditorUtilityOps.cpp, CortexEditorCommandHandler.cpp; CortexEditorUtilityTest.cpp; MCP/src/tools/qa/composites.py; BOTH `src/tools/qa/detector.py` and `src/cortex_mcp/qa/detector.py`; tests/test_qa_tools.py; controller routers.py native policy. Remove stale log/max_lines/cursor consumer forms; do not fix unrelated screenshot filename contract.
**Interfaces:** GetRecentLogs gains native limit/default200/max2000, optional inclusive UntilCursor and CountOnly; result struct has Entries, NextCursor, LatestCursor, WindowEndCursor, OldestAvailableCursor, MatchedCount, HasMore, LostBeforeCursor. Existing internal call sites migrate; no alias Cursor field. Python helper `_read_log_window(connection, start_cursor)->(entries, coverage)` used by test_step/scenario; native response entries/count/next_cursor/latest_cursor/window_end_cursor/oldest_available_cursor/matched_count/has_more/lost_before_cursor. Count-only pre-action watermark; no record strings.

- [ ] Native RED: LogCapture(4) with natural increasing entries, severity/category gaps and more matches than requested. First page cap1 has next cursor of actual consumed position, continued fixed watermark returns remaining matches once. A newly added record after window_end is excluded until next window. Evict front records; loss is explicit; empty window continuation never regresses. Use current fixture constructors/AddEntry, not a mocked list.
```cpp
FCortexEditorLogCapture Capture(8);
Capture.AddEntry(ELogVerbosity::Log, TEXT("CortexTokenTest"), TEXT("first"), 10.0);
Capture.AddEntry(ELogVerbosity::Error, TEXT("CortexTokenTest"), TEXT("fault without prefix"), 10.1);
auto Params = MakeShared<FJsonObject>();
Params->SetNumberField(TEXT("limit"), 1);
Params->SetNumberField(TEXT("since_seconds"), -1);
const auto Page = FCortexEditorUtilityOps::GetRecentLogs(Capture, Params);
bool bHasMore = false;
double NextCursor = -1, EndCursor = -1;
TestTrue(TEXT("continuation flag exists"), Page.Data->TryGetBoolField(TEXT("has_more"), bHasMore));
TestTrue(TEXT("another retained match"), bHasMore);
TestTrue(TEXT("next cursor exists"), Page.Data->TryGetNumberField(TEXT("next_cursor"), NextCursor));
TestTrue(TEXT("window end exists"), Page.Data->TryGetNumberField(TEXT("window_end_cursor"), EndCursor));
TestTrue(TEXT("last omitted match is not consumed"), NextCursor < EndCursor);
```
Python RED: canonical severity error without prefix is detected; a final/only step with201 retained records and only error on201 inspects it once or reports incomplete coverage. Add retained early-error timestamp10 followed by end-of-step timestamp70: cursor-only QA must detect it once despite a duration beyond the old30-second default. Use a finite log-window test double modeling pages/window/loss/time selection, not an echo of wrong fields; real native/public smoke proves protocol independently. Native RED calls the existing JSON utility operation, not the not-yet-added C++ signature, and uses TryGet APIs so missing new fields fail assertions without JSON warnings.
- [ ] Build/run `Cortex.Editor.Utility.LogCapture+`; Python `rtk proxy uv run pytest tests/test_qa_tools.py -v`; record old duplicate array/wrong cursor/no final-page detection RED. Do not run assertions that depend on current fake level/next_cursor fixtures.
- [ ] Native scan under existing buffer lock: select window end; since_seconds=-1 disables time filtering, nonnegative time limits anchor to the retained end timestamp, other negatives refuse. Scan only cursor<=end, count matching rows, retain at most limit (none count-only). Compute continuation without consuming omitted matches; finished scan advances through nonmatching tail. Capture loss/oldest/latest honestly including evicted windows; no whole-list copy then trimming. Serialize entries/canonical severity only, no logs/Cursor aliases. Add native metadata and controller's editor.get_recent_logs policy before public limit smoke.

Python finite consumption algorithm:
```python
baseline = connection.send_command("editor.get_recent_logs", {"count_only": True, "since_seconds": -1})["data"]["latest_cursor"]
# Execute this step's action/wait; then first page fixes the end-of-step watermark.
params = {"since_cursor": baseline, "since_seconds": -1, "limit": 200}
page = connection.send_command("editor.get_recent_logs", params)["data"]
end = page["window_end_cursor"]
entries = list(page["entries"])
while page["has_more"] and not page["lost_before_cursor"]:
    params = {"since_cursor": page["next_cursor"], "until_cursor": end, "since_seconds": -1, "limit": 200}
    page = connection.send_command("editor.get_recent_logs", params)["data"]
    entries.extend(page["entries"])
```
The implemented helper handles native/MCP size refusals, connection failures and non-progress by returning coverage.complete=false with available entries/explicit cause—never retries, polls beyond the fixed window or overwrites an applied action's success. The caller detects after collecting pages and retains per-step window/count/loss coverage. Both detector copies consume entries/severity, not logs/level. Any new log record beyond fixed end belongs to subsequent action's window; no late-stage attribution.
- [ ] Build/GREEN and actual editor_cmd log stream plus scenario_compose/qa_test_step smoke. Emit >200 task-owned test log records, last error must be reported on final step exactly once. Severity-tagged expected test error must be declared as expected; no arbitrary warning suppression. Inject ring/larger-than-one-record budget cases through native fixtures, not public production guards bypass.
- [ ] Update Editor/QA protocol/toolkit docs and task evidence; scoped commit only with authority.

## Task 12 — Reflection summaries before extraction and honest hierarchy/search

**Files:** CortexReflectOps.cpp class detail/hierarchy/search; CortexReflectCommandHandler.cpp; MCP/src/tools/reflect/detail.py/context.py/hierarchy.py; CortexReflectHierarchyTest.cpp/CortexReflectSearchTest.cpp; test_reflect_tools.py behavior.
**Interfaces:** existing native detail=summary/full; standalone summary default/properties/full valid; include_flat_classes=false default for native hierarchy; search returned/total_matches/truncated honest. Keep source/type/parent/asset/module and not_scanned semantics.

- [ ] RED: actual summary contains identity/parent but not properties/functions; explicit full/properties recovers declared members. Parent context native summary avoids full extraction; hierarchy remains navigable without duplicate flat classes. A search fixture exceeding limit has matched count distinct from returned, or explicit truncated when total cannot be cheaply proven. Selected native defaults/coverage never vanish under Python pruning.
```python
summary = json.loads(tools["query_class_detail"](class_name="AActor", detail="summary"))
assert summary["name"] == "AActor"
assert "properties" not in summary and "functions" not in summary
```
`tools` is the existing registered actual function collection with a real connection in E2E; do not repin mocked forwarded params as new evidence.
- [ ] Build/run `Cortex.Reflect+`; Python `rtk proxy uv run pytest tests/test_reflect_tools.py -v`; expect current standalone full default/flat duplicate/incomplete search count RED.
- [ ] Validate standalone detail; send native summary for summary calls and context parent. Properties view can use native full then existing `_prune_detail`; include all safe coverage/error fields outside presentation allowlists. Native hierarchy conditionally emits flat classes; existing Python flatten uses children (inspect actual collector before removal). Search scans/counts matches before limit or explicitly labels incompleteness; do not call returned length total_matches.
- [ ] GREEN and actual query_class_detail/query_class_context/hierarchy/search against task-owned classes. Cache key includes native detail and inherit flags; changing view never returns stale full/default data.
- [ ] Update Reflect docs/evidence; scoped commit only with authority.

## Task 13 — Gen job lists without repeated prompt bodies

**Files:** CortexGenCommandHandler.cpp and supported command metadata; CortexGenCommandHandlerTest.cpp/CortexGenJobManagerTest.cpp; no provider, polling cadence or job-lifecycle changes.
**Interfaces:** list_jobs include_prompt=false; true restores prompt; job_status stays full. Native limit semantics preserved, including0. Optional server gate remains live availability.

- [ ] RED in CortexGenJobManagerTest.cpp, where the existing FTestGenProvider/CreateTestJobManager fixture is visible: RegisterProvider, SubmitJob MeshFromText requests with distinct long prompts, cancel to terminal state, then construct FCortexGenCommandHandler with that actual Manager. List rows retain exact ids/status/time/progress but omit prompt by default; explicit true and job_status recover the corresponding prompt. The provider is an existing isolated TEST fixture, not a fake production provider or an external API call. Never call LoadJobs/SaveJobs in this fixture.
```cpp
FCortexGenCommandHandler Handler(Manager);
Params->SetBoolField(TEXT("include_prompt"), true);
const auto Detail = Handler.Execute(TEXT("list_jobs"), Params);
const auto* Row = Detail.Data->GetArrayField(TEXT("jobs")).FindByPredicate(
    [&JobId1](const TSharedPtr<FJsonValue>& Item) {
        return Item->AsObject()->GetStringField(TEXT("job_id")) == JobId1;
    });
TestNotNull(TEXT("submitted job retained"), Row);
if (Row)
{
    TestEqual(TEXT("prompt retained on request"), (*Row)->AsObject()->GetStringField(TEXT("prompt")),
        Manager->GetJobState(JobId1)->Prompt);
}
```
Add `#include "CortexGenCommandHandler.h"` to the existing manager test. Existing insertion path is RegisterProvider/SubmitJob, not direct mutation of private Jobs; manager header public query GetJobState verifies canonical state. Keep separate default-omission and job_status assertions; the shown full-value assertion alone is not sufficient.
- [ ] Build/run `Cortex.Gen+`; expect current unconditional prompt/default wire behavior RED.
- [ ] Gate only prompt field emission; native manager limit/filter unchanged. Add include_prompt metadata. Task3 handles native list limits; no fallback advertisement of unavailable Gen.
- [ ] GREEN plus native command smoke. If live Gen is enabled, actual gen_cmd list/job_status proves it; if absent, prove absence in tools/list and record native handler evidence/availability limitation, not a fabricated live provider pass.
- [ ] Update optional-domain docs/evidence; scoped commit only with authority.

## Task 14 — Changed-leaf class-default echoes without changing mutation/rollback

**Files:** CortexBPClassDefaultsOps.cpp: SetClassDefaults/PreflightBatch, private BuildClassDefaultEchoDiff and private FCortexClassDefaultValueSnapshot; CortexBPCommandHandler.cpp; CortexBPTokenReadTest.cpp/mutation tests. No global serializer/setter change, second public Core diff API or cross-domain echo framework.
**Interfaces:** echo=diff|full|none, default diff; type/success/changed survive. Scalar/atomic diff emits previous/new only when changed; struct diff changes use relative leaf paths. Full emits complete previous/new; none no values. Batch item echo admitted by existing preflight. FCortexClassDefaultValueSnapshot(Property, ValuePtr, Owner) captures legacy JSON plus exact scoped native storage only for roots containing precision-sensitive integer types; native CopyCompleteValue restores those roots on a later property failure. FGCObject-held references keep snapshot object values/owner/type valid during the synchronous loop; aligned initialization/destruction match the property. Release exact snapshots BEFORE explicit compile/reinstance, never keep their raw descriptor/value pointers across frames. Echo uses exact typed comparison when JSON is lossy, not metadata-only recovery of rounded numbers.

- [ ] RED: set one struct leaf while sibling values remain, default diff shows only changed leaf including false/null clear; full shows complete previous/new; none still shows authoritative outcome. All modes apply identical values, save/compile status and failure recovery. Include no-op and failure after a previously applied property to ensure journal previous values are retained.
```cpp
const FString PropertyName = TEXT("bReplicates");
auto WriteValues = MakeShared<FJsonObject>();
WriteValues->SetBoolField(PropertyName, true);
Params->SetStringField(TEXT("asset_path"), AssetPath);
Params->SetObjectField(TEXT("properties"), WriteValues);
Params->SetBoolField(TEXT("compile"), false);
Params->SetBoolField(TEXT("save"), false);
Params->SetStringField(TEXT("echo"), TEXT("none"));
const auto Set = Handler.Execute(TEXT("set_class_defaults"), Params);
const auto Property = Set.Data->GetObjectField(TEXT("results"))->GetObjectField(PropertyName);
TestTrue(TEXT("outcome survives no echo"), Property->GetBoolField(TEXT("success")));
TestFalse(TEXT("no full value echo"), Property->HasField(TEXT("previous_value")));
auto ReadParams = MakeShared<FJsonObject>();
ReadParams->SetStringField(TEXT("asset_path"), AssetPath);
ReadParams->SetArrayField(TEXT("properties"), {MakeShared<FJsonValueString>(PropertyName)});
const auto Read = Handler.Execute(TEXT("get_class_defaults"), ReadParams);
TestTrue(TEXT("no echo still applied the requested value"),
    Read.Data->GetObjectField(TEXT("properties"))->GetObjectField(PropertyName)->GetBoolField(TEXT("value")));
```
AssetPath belongs to Task6's Actor fixture, whose bReplicates starts false. For the changed-struct case add TokenVector with PC_Struct/TBaseStructure<FVector>::Get(), default (1,2,3), then set only X=4 through the native property setter and require changes.X before1/after4 with Y/Z unchanged on readback. Add exact string-case/FText/null/large-integer and atomic collection cases so the helper cannot confuse representational equality with a real mutation.

Precision RED is mandatory and local: seed test BP int64 WideCounter=9007199254740993 directly through FInt64Property in the fixture, then native setter requests exactly representable9007199254740992. Legacy PropertyToJson rounds both to the same double; revised echo must report changed=true, exact decimal before/after and value_encoding=decimal_string. Native reflection verifies the actual post-state; do not use the legacy lossy getter as the oracle. Inject a later property failure after observing the first assignment via existing property-change event, and require native WideCounter restoration to9007199254740993 for diff/full/none. Add a struct/array containing int64 so the root snapshot covers container shape. This does not promise that the unchanged legacy setter accepts arbitrary exact odd integers from TCP.

Implement the private snapshot with FProperty::InitializeValue/CopyCompleteValue/DestroyValue and owned aligned storage; visit object references through the same reflected struct/container tree and retain them through FGCObject. Use native field type and real ValuePtr, not a class-sized buffer or parent CDO layout. Ordinary roots retain the existing JSON journal. Integer echo serialization follows CortexSTInspectOps.cpp:113-121's decimal-string convention outside [-9007199254740991,9007199254740991], implemented LOCALLY without a StateTree dependency. For unsafe integer leaves encode exact decimal strings and report per-leaf value_encoding (scalar) or value_encodings paths (struct/atomic collections); do not feed those strings into the legacy JsonToProperty rollback. Snapshot native copy, not lossy JSON, restores the precision-sensitive root.
- [ ] Build/run `Cortex.Blueprint.ClassDefaults+` and TokenRead family; current unconditional full echo RED.
- [ ] Validate echo before mutation. Capture snapshot before assignment in every mode when rollback precision requires it, not only when an echo is requested; ordinary paths avoid extra native copies. Keep existing journal metadata and use exact snapshot branch in rollback. Diff traverses declared structs, strings compare case-sensitively, FText through descriptors, collections atomically. Use native typed equality for precision-sensitive roots, exact encoded before/after for echoes, and never derive changed=false from rounded JSON. If another representation is genuinely incomplete, echo_complete=false is explicit and outcome remains authoritative. Release snapshots before compile; warnings/save/guards/per-item state remain identical.
- [ ] Build/GREEN, actual write/read and explicit save/cold reload for each mode; expected save/compile failure retains same applied/rollback outcome and required error details.
- [ ] Update mutation contract docs/evidence; scoped commit only with authority.

## Task 15 — One offline Blueprint catalog, lean entry point

**Files:** MCP/src/cortex_mcp/schema_generator.py render_catalog; test_schema_generator.py/test_schema_blueprint.py; no editing existing .cortex/schema user files during implementation tests.
**Interfaces:** _catalog.md counts+relative links; render_blueprint_catalog remains full and deterministic; freshness sidecar untouched.

- [ ] RED: temporary generated schema with known parent/child classes; entry point resolves blueprints.md and the authoritative file contains exact class/parent/path data; index doesn't contain a second class table. Test navigation/identity rather than specific heading prose.
```python
catalog = schema_dir / "_catalog.md"
linked = catalog.parent / "blueprints.md"
assert linked.is_file()
assert child_asset_path in linked.read_text(encoding="utf-8")
assert child_class_name not in catalog.read_text(encoding="utf-8")
```
The fixture generates both files through real renderer/generation paths; freshness/version assertions pin consumer semantics, not copied strings.
- [ ] Run `rtk proxy uv run pytest tests/test_schema_generator.py tests/test_schema_blueprint.py -v`; current duplicate class entry RED (test exact class identity in canonical file plus absence of duplicate data, not only size/nonempty).
- [ ] Replace render_catalog class rows with overview counts and a relative link; retain data index/freshness/version/project sections. Full class catalog output unchanged. Don't alter archive JSON/schema version for a navigation-only markdown change.
- [ ] GREEN and generate into task-owned temp schema directory via real command/script; read linked file and freshness data. No overwrite of the root's dirty snapshots.
- [ ] Update schema docs/evidence; scoped commit only with authority.

## Task 16 — Same-turn context dedupe and one current access policy

**Files:** Source/CortexFrontend/Private/Widgets/SCortexInputArea.cpp/h; Providers/CortexCodexCliProvider.cpp; SCortexInputAreaTest.cpp and CortexCliSessionTest.cpp. No new context cache/service, provider lifecycle or game state API.
**Interfaces:** ResolveProviderChip optionally consumes a per-send gathered editor snapshot; successful explicit editorContext suppresses duplicate auto preamble. Current access mode still asserted exactly once for default/custom/resumed prompts.

- [ ] RED using actual Slate input widget existing TestResolveAndSend/OnSendMessage: attach provider editorContext and enable auto-context, choose a task-owned selected actor; emitted message includes its identity once, user text and other chips preserved. Next send after changing selection includes the new state, not cached old context. Disabled/empty/failed provider fallback remains. Codex custom/default/resume keeps correct access mode/policy.
```cpp
FString Captured;
auto Widget = SNew(SCortexInputArea)
    .OnSendMessage_Lambda([&Captured](const FString& Prompt) { Captured = Prompt; });
FCortexContextChip Chip;
Chip.Kind = ECortexContextChipKind::Provider;
Chip.Label = TEXT("editorContext");
Widget->AddContextChip(Chip);
Widget->TestResolveAndSend(TEXT("Inspect selection"));
```
Add exact selected-identity occurrence and next-turn state assertions after this consumer action; restore settings/selection and delete task actor even on failure. ECortexContextChipKind::Provider is declared in Private/AutoComplete/CortexAutoCompleteTypes.h:26-30 and already used by existing input tests.
- [ ] Build; use rendering UnrealEditor for `Cortex.Frontend.InputArea.Resolution+` (runner may classify Frontend-only filter as non-rendering; do not accept a Slate skip). Direct supported rendering invocation:
```bash
rtk proxy "D:/UnrealEngine/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "D:/UnrealProjects/CortexSandbox/CortexSandbox.uproject" -ExecCmds="Automation RunTests Cortex.Frontend.InputArea.Resolution" -TestExit="Automation Test Queue Empty" -unattended -NoSound -NoSplash -NoLiveCoding -log=TokenEfficiencyFrontend.log
```
Observe duplicate identity/default access-mode RED, not just count-grew or incidental wording.
- [ ] Gather once only when auto-context or explicit editorContext needs it, pass snapshot through that chip resolution, and inject exactly once on success; no allocation/copy for an unused snapshot and no cross-turn cache. Remove only duplicate default access-mode sentence while retaining appended policy for custom and resumed turns. LSP references first for helper signature changes.
- [ ] Build/GREEN, rendering-editor actual input send surface/captured payload, changed selection next turn and result-display smoke in text/structured modes. Do not initiate a paid model call merely to prove string assembly.
- [ ] Update Frontend docs/evidence; scoped commit only with authority.

## Task 17 — Consumer/schema synchronization and independent integrated acceptance

**Files:** Generated MCP/src/cortex_mcp/_fallback_generated.py and tests/fixtures/capabilities_cache_full.json after live registration; affected behavioral signature contracts; cortex-toolkit/resources/{mcp-tool-reference,data-patterns,blueprint-development,level-design,statetree-development}.md and actual domain skill references; root docs/systems/{python-mcp,cortex-core,cortex-data,cortex-blueprint,cortex-material,cortex-level,cortex-umg,cortex-editor,cortex-reflect,cortex-gen,cortex-frontend,cortex-schema}.md and INDEX; owned plugin docs/verification/2026-10-13-token-efficiency.md. Preserve pre-existing dirty root docs, skills, gitlinks and snapshots; integrate overlapping edits surgically.
**Interfaces:** Every TE01-17 mapped to A1-9 and the exact plugin/toolkit candidate identities; no stale signature/cache claim. Toolkit sync uses current repository conventions/skill; no automatic push.

- [ ] Regenerate live capabilities after a supported build/new owned Editor startup, then `rtk proxy uv run python scripts/sync_fallback.py` from MCP. Compare supported native command metadata with fixtures/fallback; do not manufacture signatures from documentation. Update explicit consumers to new class/filter/log/discovery/summary/echo names; no fallback aliases. Update only genuinely behavioral contract tests; delete incidental wording/default-shape/mock-copy tests instead of re-pinning them.
- [ ] Independent controller builds the integrated candidate and runs staged Python suites, serial native full rendering suite and required targeted lifecycle matrices:
```bash
rtk proxy uv run pytest tests/ -m "not e2e and not scenario and not stress" -v
rtk proxy uv run pytest tests/ -m "e2e" -v
rtk proxy uv run pytest tests/ -m "scenario" -v
rtk proxy uv run pytest tests/ -m "stress" -v
```
Each marker is a separate invocation; required live fixtures/skips recorded truthfully. Full native root command:
```bash
rtk proxy powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location 'cli/Testing'; .\RunTests.ps1 'Cortex+' -Timeout 600 }"
```
Use actual configured engine path and rendering; no overlapping runs/Editors. Raw warnings/expected errors classified. Build/test not sufficient alone.
- [ ] Execute manual `codex/cortex-mcp-benchmark` against the intended CortexSandbox Editor in its prescribed domain order with unique `/Game/Temp/CortexMCPTest/Bench_<timestamp>` owned assets. Exercise A1-9 PUBLIC routes on fixtures: metadata/index leaf/full/changed-only/echo+save, asset prefix/count/pages, catalog/component/job summary/full, tree cut+binding state, native versus cached page continuity, fixed-window final-step logs, reflection/schema/context and stdio toolsets/modes. Native fixtures cover optional Gen command semantics if live domain unavailable; do not invent live success or remove accepted Gen implementation. Record actual available/unavailable environments.
- [ ] Measure before/after tools/list characters and representative model-facing UTF-8 bytes on same fixture populations; measure tokenizer-specific tokens only with an explicitly named available tokenizer. Stop at A1-9 and mapped findings; do not admit speculative new matrices. No renderer-only optimization claim and no promised60-80% not measured.
- [ ] Use cortex-sync-toolkit for API propagation and cortex-sync-docs for post-implementation doc audit, preserving their approval gates and current publication authority. Update design/implementation statuses and acceptance verification report. A docs-only tail commit requires source/ancestry binding, not repeated unchanged suites. Request bounded independent implementation review against exact candidate/base/dependencies; fix supported findings and rerun only affected positive/refusal/lifecycle proof.
- [ ] Deliver verified local implementation unless user has explicitly authorized a different endpoint. No push/PR/merge/closure or branch force-deletion inferred from this plan. Unavailable required runtime is a precise integration blocker, not permission to claim done; finish reachable work and preserve evidence/user state.

## Self-review and handoff gates

- [x] All TE01-17 map to tasks/A1-9; Review Focus cases belong to named task owners.
- [x] Every planned behavior path specifies RED, GREEN and actual smoke; execution results are still unobserved.
- [x] Private helper, cursor, parent-memory, approval/outcome, orphan-binding and context-lifecycle hazards are assigned explicit preservation/proof contracts.
- [x] Controller self-review and independent scoped review resolved missing contracts, precision/time-cutoff gaps and misleading RED examples. New APIs are labeled planned; nine Python snippets parse. C++ snippets are not compiled.
- [ ] User approves reviewed artifacts/execution authority before any product implementation. Recommended execution is controller-inline for shared Python/lifecycle, independent substantive native domain slices only when contracts and file ownership are settled; controller keeps runtime acceptance. No routine workflow re-selection during an authorized execution.

## Simplicity check

- [x] Think before acting — reviewed design and exhaustive domain/mechanism audit are prerequisites; source bases and native-runtime limitations recorded.
- [x] Simplicity first — one Python boundary and explicit maps, existing read owners/leaf resolution/exports; no native framework or new standalone tools.
- [x] Surgical changes — 17 scoped tasks, exclusive owners/dependencies, caller migration and documented unrelated exclusions.
- [x] Goal-driven execution — finite A1-9, TDD/native compile/real public smoke/controller review; this is an unexecuted plan, not product completion.
