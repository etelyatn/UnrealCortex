# Issue 176 — guarded UMG animation authoring verification

Issue: https://github.com/etelyatn/UnrealCortex/issues/176

## Candidate identity

- Base: `6506e03a21a8a2c6af1570b3d01e736947cd3040`.
- Implementation parent: `96c762fe11f517897f8a795e72121fdb537bbfe7`.
- Exercised source manifest: `Saved/Issue176/verified_candidate_source_manifest.json`, 903 files, aggregate SHA256 `1a3c341fe67f5f5e66276395f6e4bb7fc8076726228dfc1844d03c798f4fdde7`.
- Final publication head and merge are recorded in the linked PR and execution ledger; documentation-only changes do not invalidate this source-bound evidence.
- Environment: CortexSandbox, Windows, UE 5.8.3, MSVC 14.44. UE 5.6/5.7 were not exercised.

## Exercised acceptance

| Acceptance | Observed evidence |
|---|---|
| Ordinary Designer binding creation, strict identities, current guards and coherent readback | Native authoring tests and actual stdio MCP authoring; live editor serves both writer schemas and detailed inspection |
| Float and FLinearColor channels, bounded keys, native clock/range, fail-closed unsupported content | Native RED/GREEN regression coverage; final integrated rendering suite |
| Canonical replacement and idempotence | Review identified inclusivity and channel-clock omissions; tests-only RED 15/17, then minimal [S,E) and per-channel clock correction |
| Explicit compile/save and exact cold-process persistence | Author 31/31; separate editor process cold 19/19; identical GUID, native content and digest |
| Saved compiled-widget playback changes properties | Public native animation play/flush/time/pause on generated widget: opacity 0.5; color [0.5,0.25,0.125,1] at 0.05s; sibling and unplayed controls stay opacity 1/white |
| Range endpoints, adjacency and natural RestoreState | Six integrated native lifecycle tests, including last valid frame and excluded endpoint |
| Reference-aware rename | Saved/compiled fixture rename preserves GUID/tracks; recompile/replay passes |
| Selective null clear and whole binding removal | Float clear retains color/binding; binding removal retains Fade and Sibling animations; native undo/redo coverage |
| Python/router contracts and bounded responses | 993 nonlive tests pass, 265 deselected; meaningful response-boundary regressions; fixture/fallback synchronization check |
| Required toolkit/system guidance | Five toolkit reference/skill/template files; approved targeted workspace UMG and INDEX changes |

### Native build and tests

Final corrected editor build: five actions, 55.25s, successful. Full rendering `Cortex+`: **1802/1802 passed**, 227.21s, including all 60 issue176 authoring/lifecycle tests.

Log: `Saved/TestLogs/AutomationTest_2026-10-08_164424.log`. Controller raw severity scan found no unexpected warnings, errors, ensures, assertions, fatal, SkipPackage, JSON or automation errors.

Canonical-equality tests-only RED: `Saved/TestLogs/AutomationTest_2026-10-08_162529.log`, 15/17 passed, two expected consumer failures. Read-only final source review approved the two corrections without reopening the design.

An earlier lifecycle fixture renamed before its first compile and caused native variable-GUID validation diagnostics. The fixture now compiles/saves before rename; production rename behavior and warning handling were not changed.

### Python

Final integrated nonlive suite: **993 passed**, 265 deselected, 6.87s. JUnit: `Saved/Issue176/mcp_final_nonlive.xml`. Generated fallback `--from-fixture --check` returned OK.

### Actual stdio MCP and cold runtime

- `Saved/Issue176/author_acceptance_final.json`: 31/31, editor PID 79464.
- `Saved/Issue176/cold_acceptance_final.json`: 19/19, fresh PID 53400, port 8743.
- `Saved/Issue176/author_manifest_final.json`: exact persisted identity/content.
- Animation GUID: `{010B8443-479F-66A2-EC2B-618BEDCDDEA7}`.
- Content digest: `5f1d65f62445dc2b599a1a085b9852fca13f5085dca8f6b300297016e4467223`.
- Native tick resolution 60000; section [0,6000), duration 0.1s.

Playback used the saved Blueprint's generated class and native WidgetLibrary Create, not direct opacity/color writes. Actual animation playback plus fixed-time observation proved the changes and unchanged controls.

An earlier cold attempt returned BLUEPRINT_NOT_FOUND because `cli/Testing/RunTests.ps1:439-451` deletes `Content/Temp/Cortex*Test*` before and after runs. Disk absence and runner source proved a harness-ordering collision. The fixture was recreated once after the final native suite, then author/cold acceptance passed without an intervening runner. Failed preliminary evidence was not relabeled as passing.

## Prescribed live cross-domain benchmark

Actual SDK calls targeted only owned CortexSandbox PID 53400/8743, not the unrelated Ripper editor on 8742. **249 benchmark calls**, summed tool wall time 17858.121ms, excluding planning/startup. Breakdown: Data/Animation 35; asset domains 146; controller 68. Summary: `Saved/Issue176/benchmark_final_summary.json`; raw packets and domain reports under `Saved/Issue176/benchmark_*`.

Exercised core/schema/health; DataTable add/read/delete and GameplayTag checks; ordered StringTable dry/apply/restore; Animation notify dry/add/update/remove; ordinary/custom Blueprint parents, variables, functions, inheritance and compile/save; guarded graph patch/readback/layout; StateTree schema/states/transition/check/compile/save; custom Widget Blueprint hierarchy/text/font/color; Material vector parameter/instance override; editor Python/next-tick/error/CVar/viewport/logs; level mesh/light/transforms/components/tags/folder/selection/attach/detach/hidden state; reflection hierarchy/detail/context/overrides/cache; project schema output; PIE start/pause/resume/stop.

Controller viewed actual surfaces:

- `Saved/Issue176/benchmark_graph_ui_fitted.png`: all eight graph nodes and five edges, fitted/readable.
- `Saved/Issue176/benchmark_widget_ui.png`: Designer green BenchmarkTitle and PressMe.
- `Saved/Issue176/benchmark_material_instance_ui.png`: orange BenchColor override, blue parent thumbnail and lit preview.
- `Saved/Issue176/benchmark_material_contrast.png`: actual blue parent versus orange instance cubes in the lit scene.
- `Saved/Issue176/benchmark_scene.png`: actual mesh/light scene.

StateTree validation was read from `validation.valid`, not a nonexistent top-level `valid`; material override from actual list/get-instance payloads. A controller assertion-shape mistake was corrected without rerunning or expanding the product checks.

Skipped positive reference claims: no benchmark StringTable references; no external benchmark Blueprint referencers for query_usages. Empty scans are not asserted as positive reference resolution.

Nonblocking observations outside issue176 scope:

1. StateTree unsaved dirty structural edits return an identical generic fingerprint until compile/save. Benchmark fingerprint presence and structure validation passed; this native owner was not changed here.
2. MaterialResult is accepted by connection operations but its virtual ID is rejected by get_node_pins. This native owner was not changed here.

Canonical full-object graph paths and supported reflection class selectors corrected initial input refusals. GUI-provider captures did not prove focused editor pixels; verified owned-window activation and DPI-aware native capture supplied the actual viewed images. No unrelated production fixes or speculative tests were added.

## Cleanup and limits

- Feature-manifest cleanup: 5/5 passed, `Saved/Issue176/cleanup_acceptance_final.json`.
- Benchmark cleanup: 13 successful actual calls, `Saved/Issue176/benchmark_cleanup_responses.json`; owned actors 0 and exact folder remaining assets 0.
- Task editor PID 53400 exited gracefully with code 0; unrelated editor preserved.
- Existing benchmark DataTable/StringTable/animation content was restored without saving those packages. Existing disk packages and user work were not deliberately changed.
- Benchmark schema was generated before scratch cleanup; refresh the project schema in the next normal editor session to remove obsolete scratch entries.
- Native/Python tests and actual MCP/runtime evidence are bound to the source manifest above. Publication additionally checks ancestry and exact candidate identity.
