# Issue 167 — serialized UMG property bindings

## Authority and candidate

- Request/reporter: [UnrealCortex #167](https://github.com/etelyatn/UnrealCortex/issues/167), @etelyatn.
- User approved end-to-end delivery and expanded exact removal into one fully implemented nullable setter: object creates/replaces; explicit null clears. Existing `umg_cmd`, `get_widget` and `get_tree` remain the public surfaces.
- Plugin base: `c05e89bd8aaae5b21af7078ab1a4dfbbeeb8490e`.
- Native implementation: `9f8d6de1315beff3c48e3fb181e1f689a546ffc8`.
- MCP implementation/runtime candidate: `4610abdb355f5f4241e7e6557bc9a42c4d7ef8bc`.
- Toolkit base: `425166b18d66849eb9af6430dd2e3fe4e9f1f365`; scoped binding guidance is on `docs/issue-167-property-bindings`.
- Scoped documentation candidates: toolkit `58a6ab572db048924eb173c1102e8b8ca3f302a2`; CortexSandbox systems-only `d5ea6e2401cfcbd990d491e8fba05deca5b1743b` (base `033a2f4b465f081dc373596c369678da0eb46e5e`).
- Prior runtime source identities (`4610abd`): Source tree `ec6348f3767f8d5e59aad739180abbb1eb59f003`, MCP tree `76bc8bd36e651f34664bae917cd9d0d61cbe5907`, uplugin blob `baf99366d84d3a09ead256639f04e779806df0bc`. An exit0 diff bound documentation candidate `54f7f550cab3c0e5dcea24161b33dce65665b7da` to unchanged prior implementation.
- Environment: Windows x64, installed UE 5.8.3 build 58210709, supported MSVC 14.44, project `CortexSandbox`. Verification builds use `-NoLiveCoding -NoHotReloadFromIDE`.
- Delivery: [UnrealCortex PR169](https://github.com/etelyatn/UnrealCortex/pull/169) confirmed MERGED as `8f6012a5536b880912f59eadd4e06ad8be7549d0`; issue167 confirmed CLOSED/COMPLETED. Full expected-head protection used on `562373b4fcb4a234655dc4f0ab104dc3199c6a1f`, unchanged base `c05e89bd8aaae5b21af7078ab1a4dfbbeeb8490e`, empty check rollup. Exit0 Source/MCP/uplugin diff against tested `3801aaa80d23419757e7999aca3923959c1d1b81` and verified ancestry bind shipped behavior to acceptance. Optional all-domain native failure stays open in #168, not claimed green.

## Change record

`CortexUMG` now reads every ordered serialized `FDelegateEditorBinding` field, including raw path-segment owner/name/GUID/discriminator identity even when resolution fails. Opt-in inspection attaches completeness, scope/counts and a whole-array fingerprint. A private native owner validates reflection metadata and authored sources, performs one exact transactional mutation, verifies the entire post-array before notification, and restores/verifies the original array on failure. Compilation and saving are separate existing commands.

The MCP router rejects pagination before cursor interception. Bounded response handling preserves counts/fingerprints and honest false completeness; an oversized mutation response preserves the actual mutation outcome rather than pretending it refused. Live capabilities and generated fallback contracts describe nullable authoring without adding a standalone MCP tool.

## Acceptance matrix

| Requirement | Exercised evidence |
|---|---|
| Clear `ProgressDisplay.Percent` without replacing its ProgressBar | Routed native clear preserves UObject identity; real MCP clear readback leaves the named ProgressBar and exact hierarchy. |
| Preserve every unrelated serialized binding and order | Native full-record comparisons; immediate live clear and fresh-process readback exactly match the current compiled/saved retained baseline. |
| Preserve Designer defaults/styles/animation bindings | Native fixture snapshots include Percent, style RGBA, hierarchy, animation binding GUIDs/possessables; live Percent 0.25, style and animation readback remain unchanged. |
| No stale serialized reference to retired `ElapsedValue` | Inspect clear before deleting the variable; explicit compile/save; fresh Editor readback contains neither source reference nor variable. No compiler sanitization is used as removal proof. |
| Missing, ambiguous, mismatched requests are non-destructive | Native missing widget/invalid attribute/duplicate exact target, absent/partial/stale/dirty-to-dirty/CRC guards; malformed/coerced JSON and native name bounds; entire array/status/dirty state unchanged. Live refusal readback retains exact array and fingerprint. |
| Incomplete reader cannot report empty success | Typed reflection schema tests fail closed for unavailable/mistyped metadata; oversized orphan inspection preserves guard/counts with `reader_complete=false` and explicit error. |
| Saved/reloaded state confirms explicit lifecycle | Native package unload/reload regression and real clean process transition from Editor 71624 to Editor 25488. |
| Object payload creates/replaces; null alone clears | Native and actual stdio MCP property creation, compatible function replacement, legacy SourceProperty retirement, identical no-op and clear. Omitted/empty/string payloads refuse. |
| Validate nested owners, signatures, purity and policy | Native nested struct/object traversal, scalar/container refusal, missing skeleton, incompatible/impure function and effective UMG project-policy coverage; null repair remains available. |
| No implicit compile/save/graph or dependent mutation | Mutation response `compiled=false`, `saved=false`; physical persistence requires separate compile/save calls. Existing getters/functions are referenced; no getter graph is generated. |
| Undo/redo and no-op/refusal transaction discipline | Exact original array restored by undo/redo; no-op and refusal placed after real clear do not consume an undo step. |
| Existing router/reads only | Fresh stdio `tools/list` exposes one `umg_cmd`; new setter appears in native command metadata, not as standalone list/remove/set tools. |

## Native RED/GREEN and build

Supported MSVC builds succeeded after the implementation and final fixture assertions. Initial feature-missing RED: five routed tests failed on the absent feature (`AutomationTest_2026-10-06_225558.log`). Subsequent targeted RED exposed an omitted compiled CRC check (`233141`), JSON getter coercion admitting a destructive clear (`233715`), a real oversized FName assertion (`234240`), and generic dispatcher guard lookup before name validation (`235116`). These are observed historical runner logs, subject to its ten-log retention policy; the initial RED file has rotated away.

Final focused feature run: **12/12**, `Saved/TestLogs/AutomationTest_2026-10-06_235401.log`. Final formatted native domain run: **80/80**, `235854.log`. Task-1 completion gate: **80/80**, `Saved/TestLogs/AutomationTest_2026-10-07_001116.log`; raw warning/error/fatal/ensure search returned **zero matches**. The baseline domain contained 68 tests; twelve feature tests are registered, including persistence and authoring-policy coverage.

Task-3 final completion gate: **80/80**, `Saved/TestLogs/AutomationTest_2026-10-07_083024.log`, zero raw warning/error/fatal/ensure matches.

Fixture corrections were root fixes, not suppression: duplicate records copied before TArray reallocation; animation GUID map initialized before compilation; loaders reset before persisted file deletion; binding policy configured through the effective `UUMGEditorProjectSettings` CDO.

### Optional broad native run — failed

`RunTests.ps1 'Cortex+' -Timeout 600` selected rendering and no NullRHI. `Saved/TestLogs/AutomationTest_2026-10-07_075116.log` recorded **204 completed successes / 205 started**, then an access violation in `UE::GC::TBatchDispatcher<UE::GC::TReachabilityProcessor<5>>::FlushQueuedReferences` on Foreground Worker #0 while `Cortex.Blueprint.RemoveGraph.Apply.RecoveryFaults` compiled its initial `BP_Recovery` fixture. It did not time out. Two earlier animation warnings were logged (`Num Curves: 1`; invalid compressed animation data on a disposable curve fixture).

The queue had not reached UMG feature tests. This establishes the observed boundary, not the cause or an independently proven baseline defect. No GC mode was disabled, warning suppressed, or unrelated production code changed to obtain a pass. Investigation and release disposition remain explicit.

Isolated Blueprint diagnosis passed **242/242** (`075637.log`) with one unrelated Google connectivity timeout warning. Early-attached CodeLLDB reached the exact initial compile breakpoint, then passed that boundary without reproducing the GC crash; debugger overhead later produced PIE timing failures. This is not release proof or a root-cause diagnosis. Safe detach timed out; API shutdown was unreachable after native test transport changes, so only the identity-verified debugger-owned PID29200 was terminated. The user's task-caused-versus-separate-issue instruction is applied below.

Controller and the independent reviewer found no reachable changed UObject mutation/global-GC path before that boundary: affected crash-domain sources are unchanged, UMG startup only registers the handler, new fixtures construct inside RunTest, the opt-in reader exits before reflection/hash, and OpenSSL is an external static-link dependency. Structural independence is strong evidence, not proof of the unknown cause. Per the user's conditional instruction, the separate investigation is [UnrealCortex #168](https://github.com/etelyatn/UnrealCortex/issues/168), confirmed OPEN.

## Final review and one correction pass

One fresh-context independent reviewer (`openai-codex/gpt-6.1-sol:high`) reviewed plugin `54f7f550cab3c0e5dcea24161b33dce65665b7da`, toolkit and systems documentation. Two Important/P2 findings; no Critical or Minor findings. Controller inspected the cited sources and confirmed both defects:

1. Prior `CortexUMGPropertyBindingOps.cpp:481-487,510` used the existing case-insensitive lookup/string matching, so lowercase `progressdisplay` could clear `ProgressDisplay`. Native RED `085553.log`: the expected WIDGET_NOT_FOUND was empty because mutation succeeded. The existing lookup now accepts an explicit case policy; the new binding owner selects case-sensitive lookup, matching, filtering and orphan diagnostics without a second traversal or extra name-copy. Other widget operations retain their existing case-tolerant lookup policy.
2. Prior `MCP/src/cortex_mcp/tools/routers.py:244-248` recognized only literal True before generic cursor routing. Twelve actual PaginationCache regressions returned cached DataTable pages for malformed flags instead of INVALID_FIELD. The router now rejects present non-boolean inspection flags before cached pagination, preserving absent/false behavior.

Correction/runtime candidate: `3801aaa80d23419757e7999aca3923959c1d1b81`. Source tree `072da439eaf1fbc4e7de8bf67cb1cabb826503f1`, MCP tree `6c279e4116e68f383360f90b47ad396deb7b3d5d`; uplugin unchanged. Supported build succeeded. Complete native UMG **81/81** (`090047.log`), zero raw diagnostics; complete non-live MCP **970 passed, 265 deselected**. Actual stdio MCP in owned Editor37208 verified two case-mismatched clear refusals, exact unchanged state/guard, twelve malformed flag refusals against a real cached DataTable cursor, and the complete author/replace/clear/compile/save/reload/source-retirement lifecycle. Complete refreshed live suite **263 passed, two same explicit skips, 970 deselected**, 170.89s. Editor37208 clean shutdown exit0; fresh Editor73256 verified persisted source absence and retained raw bindings/tree/defaults/style/animation plus strict guard refusals. Physical saved file30682bytes SHA256 `5cc484dd107eaee2ea0f36032e00fb03b6cc914983eea057cf5569f24edb49ab` matched after the live suite and first process exit. Exact smoke asset then deleted through actual MCP; file absent, Editor73256 clean shutdown exit0. Durable [post-review evidence](assets/issue167/review-fix-evidence.json):69 create records/checks,18 fresh-reload records/checks.

## Python contract and live suites

| Run | Result |
|---|---|
| Focused affected contract/router/response suite | **114 passed** |
| Non-live complete suite and Task-2 gate | **958 passed, 265 deselected** |
| Prior actual live E2E (`-v -rs`, port8743) | **263 passed, 2 skipped, 958 deselected**, 234.94s |
| Scenario stage | **59 passed** |
| Stress stage | **14 passed** |
| Generated fallback consistency check | Up-to-date |
| Final post-review non-live complete suite | **970 passed, 265 deselected** |
| Final post-review actual E2E/scenario/stress selection (`-v -rs`, port8742) | **263 passed, 2 skipped, 970 deselected**, 170.89s |

Markers overlap: these are stage totals, not additive unique tests. Final E2E skips: `DataLayerEditorSubsystem unavailable in current editor context` and `save_all is unstable in unattended e2e (can block on asset save workflows)`. Existing pytest failure cache contains stale/non-current node IDs; current full command output is authoritative. Coverage percentage was not measured.

`uv run --no-sync` uses the already installed environment because synchronization tries replacing the live MCP executable. Unique task-owned `--basetemp` avoids access denied to the existing Windows pytest temp root; permissions and unrelated artifacts were not changed.

## Real MCP persistence and identity

Actual fresh stdio `python -m cortex_mcp.server` processes used explicit project root and port8743, not an assumed mounted server connection. Task-owned Editor PIDs were verified before shutdown; unrelated Editor8742 was preserved.

Final smoke asset: `/Game/Temp/Issue167/Smoke_20261007_000640/WBP_BindingSmoke`.

1. Create generic Widget Blueprint, Canvas root and two ProgressBars; author float source and animation fixture using supported operations/trusted fixture setup, not Python access to protected Bindings.
2. Inspect, create property binding, replace with existing compatible function, check idempotence/refusals; explicitly compile/save and reload.
3. Restore property source, inspect current retained baseline, clear exactly Percent and read serialized absence before deleting source variable.
4. Explicitly compile/save; physical file **30,682 bytes**, SHA256 `e61ff6cf03838b80f49c15df99daab60768a9cbb274d73353d553d193373cdf8`.
5. Identity-checked clean shutdown of Editor71624; independent certutil hash matched. Fresh Editor25488 confirmed retained raw records, widgets/default/style/animations and retired source absence.
6. Wrong CRC, missing CRC, numeric guard values, boolean signature version, invalid read flag and oversized target/widget/asset names refused without changing serialized records or fingerprint.
7. After final E2E, physical bytes/hash still matched. User-approved exact fixture deletion then removed the smoke asset.

Local evidence: `Saved/Issue167LiveEvidence_create_fresh.json`, `Saved/Issue167LiveEvidence_reload_fresh.json`, `Saved/Issue167LiveManifest.json`. First earlier smoke asset was later absent on disk; the deletion cause is unestablished. Its failed fresh-load evidence is retained, not attributed to the product or pytest. Assertion corrections: NAME_None renders as `"None"`; compilation can normalize path-owner classes, so retained identity compares the current compiled/saved baseline.

Durable actual call results, manifests, source identities and benchmark dispositions: [assets/issue167/evidence.json](assets/issue167/evidence.json). The native runner's cleanup explicitly deletes `Content/Temp/Cortex*Test*`, including the first smoke prefix; this is consistent with the earlier disappearance, but no per-folder deletion log establishes that individual event.

## Manual all-domain MCP benchmark

**116 measured calls; 114 passed, two existing semantic mismatches; active call time 14,123.31ms.** Expected invalid-input refusals count as pass. Setup and cleanup calls are recorded separately in the same local evidence. The operator session had an interrupted shell call; active MCP timing is not whole-session elapsed time.

| Domain/check | Disposition |
|---|---|
| Connection/status | CortexSandbox, plugin0.3.5, owned PID25488 |
| Data/tag/row | Known valid/invalid tags; own row add/read/delete; exact original rows restored without save |
| Localization | Clone only; ordered dry-run/apply/copy/prefix migration and exact translations; limit50 scan succeeds; reference-field check **SKIP: no benchmark references** |
| Blueprint | Generic Actor and CortexBenchmarkActor child variables/function/compile; inherited GetBenchmarkScore visible |
| Graph | Fresh custom-parent mixed exec/data graph, eight nodes/four exact edges, stable full layout, clean compile; initial documented Branch.True graph failed and stopped |
| StateTree | Root/Idle/Chase plus transition; fingerprint on mutations/validation/compile; structure and compile valid |
| Animation | Original sequence dry-run leaves notifies/fingerprint unchanged; duplicate add/update/exact-selector/remove cycle ends empty |
| UMG | CortexBenchmarkWidget, five-widget structured tree, title/font/color readback, inherited GetWidgetVersion, clean compile |
| Material | One parameter node/edge and instance RGBA override; actual blue parent/orange instance contrast visible; verifier spelling mismatch |
| Editor | Output and next-tick Python; legacy defer refusal; bounded exception; restored CVar/mode; actual PIE start/pause/resume/stop and state readback |
| Level | Three own actors; transform/property roundtrip/components/tags/folder/attach/detach/find/select readback |
| Reflect | Health/scan/hierarchy/detail/context/overrides/rebuild complete; usages query returned zero referrer candidates, not positive usage proof |
| Schema | All-domain generation errors[]; catalog and populated Blueprint domain file read back |
| Visual | Designer and real colored scene verified; **graph readability explicitly waived by user**, API-only nodes/edges/layout/compile evidence accepted |

Existing mismatches logged immediately in `Saved/mcp_benchmark_issues_20261007_001759.md`: (1) Medium: Branch display label `True` documented as executable native pin (actual `then`); initial composite deleted its partial asset, a separate fresh valid graph used native describe_node contract. (2) Low: material composite verifier compares `MSM_Unlit` to native `Unlit` despite correct graph/override. Neither was silently fixed in issue167.

Windows refused owned-window activation. Orca returned occluded unrelated foreground pixels despite restore; those pixels were not Unreal proof and no input was sent to that app. Target-only Win32 PrintWindow verified the Designer. User chose API-only graph evidence and deletion of task fixtures. Three exact recorded actors and ten recorded assets, including smoke, were removed in dependency order; actor lookup returned zero and smoke file absent. Owned Editor25488 then shut down cleanly with exit0; unrelated Editor8742 untouched.

## Documentation, limits and rulings

Scoped toolkit guidance: `resources/umg-patterns.md`, `resources/ui-development.md`, `resources/mcp-tool-reference.md`, `templates/domains/umg.md`. Project-side system references: `docs/systems/cortex-umg.md`, `docs/systems/INDEX.md` in CortexSandbox. Adapter inventories/README tool counts remain unchanged because no new MCP tool exists. No matching request or tech-debt entry was completed by this issue.

Owning documentation merged with full expected-head protection: [cortex-toolkit PR66](https://github.com/etelyatn/cortex-toolkit/pull/66), merge `660a3e9cd3a4936ca672f8e3af7dfbf87d1fd780`; [CortexSandbox PR116](https://github.com/etelyatn/CortexSandbox/pull/116), systems-only merge `af9e6aaaddbb373b5bea7aab152e57e5316abd51`. Both remote states confirmed MERGED; neither closes the implementation issue alone.

UE5.6–5.7 and non-Windows builds were not exercised. Native schema validation fails closed; that is not proof of those platform builds. Native restore helper was exercised directly because no public failure-injection API was added solely to manufacture post-write reader failure. The final reviewer inspected success-boundary ordering and recovery failure blocking; no additional finding was submitted.

Rulings, in order:
1. Explicit Windows Git Bash for skill scripts; default bash is another environment. Cost if wrong: execution environment mismatch.
2. Engine OpenSSL private dependency for SHA256; generic platform SHA256 asserts here, existing Core helper is file-only. Cost: unexercised platform build portability.
3. Existing uv environment/no-sync plus unique pytest temp roots. Cost: dependency drift; supported environment suites passed.
4. Toolkit resource/template sync without new adapter tool inventories. Cost: stale discovery; actual tools/list verified.
5. Native persistence fixture committed with native owner, while Task3 retained runtime/integration gates. Cost: task review ranges overlap.
6. User-approved API-only graph evidence after visual activation refusal. Cost: graph readability not visually proven.
7. User-approved exact task fixture cleanup. Cost: disposable inspection assets unavailable; evidence retained.
8. Task3 implementation/runtime/docs gate precedes the one whole-branch review/publication, resolving the brief/skill task-boundary conflict. Cost: administrative boundary differs, not release acceptance.
9. User's conditional broad-failure disposition applied using source-backed structural independence and independent review; unresolved root cause tracked in #168. Cost: an unobserved causal link could require revised task attribution; no baseline or broad-green claim.
10. Extend the existing private widget lookup with an explicit case policy for the binding owner; preserve other operations' lookup policy and avoid a duplicate traversal/name-copy. Cost: case-policy misuse at a future caller; exact-target regression exercises refusals, filtering, orphan diagnosis and retained raw identity.

Deferred review minors: **none**. The two unrelated benchmark semantic mismatches and #168 are separately recorded diagnostic limits, not silently dropped issue167 acceptance.
