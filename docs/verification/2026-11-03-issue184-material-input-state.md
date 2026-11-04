# Issue184 — material root input state and effective-state diagnostics

## Disposition and identity

- Request: https://github.com/etelyatn/UnrealCortex/issues/184 (reporter etelyatn).
- Route: local implementation, PR delivery, merged integration; parent CortexSandbox gitlinks and pre-existing parent edits excluded.
- Plugin base: `c1499002cf4cc53825f55361b39753f9b7413137`. Approved design + implementation plan: `75312b22`.
- Plugin branch: `fix/material-input-state-184`. Toolkit branch: `fix/material-input-state-184` from `origin/main` `cc901ae0159f58813f4e48214ff55c86c8cb0bdc`.
- Candidate source identity (SHA-256). The repository blobs are LF while `core.autocrlf=true` materialises CRLF in the working tree, so both forms are recorded: `checkout` is what a fresh `git switch` produces and what `sha256sum` reports locally; `blob` is the committed content (`git show <rev>:<path>` with CRLF stripped). They differ only in line endings.
  - `Source/CortexMaterial/Private/Operations/CortexMaterialAssetOps.cpp`
    checkout `b5930d637ca4c64ff93902ec04dab2c4c1b07b4400824ddfaff6f9f894f5c886`, blob `756d2d431beebcc220cb32dadfe87f9953a492e4dd5adf1f857274ae00d44375`
  - `Source/CortexMaterial/Private/Operations/CortexMaterialInputUtils.cpp`
    checkout `68938fb47ae7e385f996e3a97f2e5beb684dc17625fa1854ad9eab1baed8de29`, blob `f2d487200aea01d7b0b8828bf96111e31ebfda8847113f18f49556ac8701f3b4`
  - `Source/CortexMaterial/Private/Tests/CortexMaterialInputStateTest.cpp`
    checkout `5885dee83ce750412ab984c254e40ba3ebefafcc3efddcb357b9ec0aa776a73e`, blob `6386a474554d09b0d322b3e699723689b6995d7628f06d25cf5381a5884565b9`

## What shipped

- New private helper `Operations/CortexMaterialInputUtils.{h,cpp}`: canonical root-input resolution, typed state projection (`connected`, `source_node`, `output_index`, `mask*`, `supports_constant`, `inline_constant_allowed`, `use_constant`, `constant`), engine-matching inline-constant policy, and effective-state diagnostics.
- `material_cmd`: new routed `set_material_input`; `get_material`/`list_connections` accept `include_input_state`; `get_material` accepts selected `properties`; `set_material_property` accepts owned dotted paths including `EditorOnlyData.*`; `connect`/`disconnect` copy/re-derive masks and apply the editor's `UseConstant` policy.
- MCP: complete-or-refuse policy for the two extended material reads, plus synchronized capabilities fixture and generated fallback. The `get_material` tool description was corrected to list the two additional authored blend names and to describe `effective_blend_mode`/`can_masked_be_assumed_opaque`; it previously enumerated only the five legacy names.
- Documentation: plugin design/plan/verification, `docs/systems/cortex-material.md`, and toolkit `resources/material-development.md` + `resources/mcp-tool-reference.md`.

## Acceptance matrix

| Criterion | Result | Evidence |
|---|---|---|
| Root inputs exposed with typed state, masks and constant policy | PASS | Live `get_material(include_input_state=true)` round-trip over EmissiveColor/Roughness/Normal/CustomizedUVs0; native `Cortex.Material.InputState.TypedPatch` |
| Texture output masks copied from the selected output, not merely stored | PASS | Public `material.connect` with `source_output=R` produced `mask=1, mask_r=1, mask_g=0, mask_b=0, mask_a=0`; native `Cortex.Material.InputState.Rewire` |
| Disconnect policy matches the Material Editor | PASS | Public `material.disconnect` on `Material` alias: connected=false, output_index=0, use_constant=true, stored constant preserved; native `Cortex.Material.InputState.Rewire` |
| Engine-disabled inline inputs are not treated as scalar inputs | PASS | WorldPositionOffset/Anisotropy/PixelDepthOffset/Displacement negatives in `Cortex.Material.InputState.DisabledInlinePolicy` |
| Constant type/shape validation before mutation; failures preserve state | PASS | `Cortex.Material.InputState.RefusalPreservesState`; live refusal loop kept the package fingerprint unchanged |
| Root leaf writes through generic paths cannot bypass typed validation | PASS | `Cortex.Material.InputState.AlternateRootRoute` (representable leaves canonicalize to the typed patch; whole-input, `Expression`, `InputName` and whole-`EditorOnlyData` replacement refuse before conversion) |
| Nested owned property paths, external assets not writable | PASS | Live nested `EditorOnlyData.OpacityMask.Constant` / `EditorOnlyData.BaseColor.Constant.r` writes; `Cortex.Material.InputState.NestedProperties` positive coverage and `NestedRefusalPreservesState` external-owner/unknown/null-hop refusal |
| Selected reads refuse unsupported storage instead of publishing null | PASS | `Cortex.Material.InputState.SelectedReadSupport` (mixed selector → `INVALID_FIELD`, no partial projection; `PhysMaterial` null still reads as null) |
| Native integer/byte conversion is range-checked before conversion | PASS | `Cortex.Material.InputState.NestedIntegerBounds` (int32/int64 bounds, uint8 0..255, other widths refused) |
| Diagnostics account for the active translator and never mutate | PASS | `Cortex.Material.InputState.Diagnostics` + `DiagnosticsNegatives`; live new-translator negative check produced no `CONNECTED_EXPRESSION_IGNORED` |
| Effective vs authored blend mismatch is reported and repairable | PASS | Live `BLEND_MODE_MISMATCH` with `can_masked_be_assumed_opaque`; clearing it removed the warning and restored masked rendering |
| Undo/redo restores input state and editor data | PASS | `Cortex.Material.InputState.UndoRedo` |
| Explicit save only; no implicit save on mutation | PASS | Live save/readback: `save_asset` reported `was_dirty=true, saved=true`; mutations alone left the package dirty and unsaved |
| Cold restart preserves state and topology | PASS | Editor restarted; `reload_asset` reported `discarded_changes=false` and the reloaded snapshot equals the saved fixture |
| Oversized reads refuse instead of truncating | PASS | `MCP/tests/test_response.py::TestMaterialInputStateResponsePolicy` (45/45 in that file) |
| Discovery/schema synchronization | PASS | Live `core.get_operation_schema` returns the new command and selectors; `get_status` reports plugin 0.3.8 |
| No unintended warnings/errors | PASS | Native log scanned for severity-tagged Warning/Error/Fatal and failed tests: no matches |

## Verification runs

| Check | Command | Result |
|---|---|---|
| Supported full editor build | `UnrealBuildTool CortexSandboxEditor Win64 Development ... -WaitMutex -FromMsBuild` | Succeeded; Compile + Link + WriteMetadata for the fresh module |
| Native material suite | `RunTests.ps1 'Cortex.Material+' -Timeout 600` | 79/79 PASS — `Saved/TestLogs/AutomationTest_2026-11-04_091738.log` (final revision; zero severity-tagged Warning/Error/Fatal) |
| Focused input-state suite | `RunTests.ps1 'Cortex.Material.InputState+' -Timeout 600` | 13/13 PASS — `Saved/TestLogs/AutomationTest_2026-11-03_224459.log` |
| MCP response policy | `uv run pytest tests/test_response.py -v` | 45/45 PASS |
| MCP staged unit suite | `uv run pytest tests/ -m "not e2e and not scenario and not stress" -v` | 1105/1105 PASS |
| Toolkit contract suites | `tests/test-skill-taxonomy.sh`, `tests/test-codex-plugin.sh`, `tests/test-opencode-plugin.sh` | PASS on final toolkit source |
| Public-route material lifecycle | `Saved/Issue184/issue184_material_smoke.py --phase setup\|connected\|override\|repaired\|reload-check` | PASS; evidence `Saved/Issue184/rendering/evidence_*.json` |
| Live Cortex benchmark | manual pass across data/graph/blueprint/statetree/umg/anim/material/editor/level/reflect/schema | Recorded in `Saved/Issue184/benchmark_20261103_225822.json`; issues in `Saved/mcp_benchmark_issues_20261103_225822.md` |

### Red/green history (TDD)

Each production change was written against an observed failing test:

1. Original root-state/rewrite gaps — RED `Saved/TestLogs/AutomationTest_2026-11-03_205400.log`, then 76/76 GREEN plus the documented blend-naming fix.
2. Independent review of task 2 found three reachable defects (alternate owned route bypassing typed root validation, nested native integer narrowing, unsupported selected scalars serialized as null) — RED 10/13 in `AutomationTest_2026-11-03_223215.log`.
3. The whole-editor-data alias assertion was initially vacuous because the alias prefix already ended with a dot; after correcting the path the alias write was genuinely accepted — RED `AutomationTest_2026-11-03_225307.log` — and the guard was added.
4. Final candidate 79/79 GREEN in `AutomationTest_2026-11-03_230132.log`.
5. Independent whole-candidate review asked for two corrections: this report's test-name citations, and a restored OpenCode `/mcp` ban that did not need removing. It also found that `set_material_input` echoed the loaded object path instead of the caller's request path like every sibling mutation. After fixing all three, the final revision rebuilt clean and passed 79/79 again — `AutomationTest_2026-11-04_091738.log`, final `CortexMaterialInputUtils.cpp` SHA-256 above.

## Live visual checks

Inspected personally on the final candidate:

- `Saved/Issue184/rendering/20261103_230705_setup_visible_inline_constant.png` — red sphere, inline constant visible.
- `..._connected_invisible_black_mask.png` — sphere clipped (black mask texture connected).
- `..._override_visible_blend_mismatch.png` — sphere visible again because the material is effectively Opaque.
- `..._repaired_invisible_after_repair.png` — sphere clipped again after clearing the opaque override.
- `Saved/Issue184/benchmark_20261103_225822_red.png` / `_green.png` — default vector parameter vs instance override render as visibly different colours.
- `Saved/Issue184/benchmark_20261103_225822_blueprint_ui.png` — benchmark EventGraph placed, branched and wired.
- `Saved/Issue184/benchmark_20261103_225822_widget_ui.png` — widget tree with title, button and label rendered in the Designer.
- `Saved/Issue184/benchmark_20261103_225822_cpp_parent_ui.png` — custom-parent Blueprint showing `Parent class: Cortex Benchmark Actor`.

An earlier capture round showed the default checker material because the engine had not finished compiling the fixture's shader jobs; `editor.run_python` + `MaterialEditingLibrary.get_statistics()` (submit + `FinishCompilation`, no PostEdit and no recompile of authored data) is now used as a read-only completion barrier before each capture. Those early images were not accepted as rendering proof.

## Limits and diagnostics

- Rendering was verified on the editor viewport with the fixture material; no packaged build, no non-Windows platform, and no UE 5.6/5.7 execution was performed. Compatibility with 5.6+ rests on the engine API surface used (no 5.8-only symbols introduced).
- The benchmark's composite verification warning (`BLEND_Opaque`/`MSM_Unlit` compared with `Opaque`/`Unlit`) is pre-existing: `MCP/src/cortex_mcp/verification/material.py` and `MCP/src/tools/material/composites.py` are byte-identical to the base commit. It is logged in `Saved/mcp_benchmark_issues_20261103_225822.md` and deliberately not fixed here.
- StateTree fingerprints are returned after every mutation/validation, but an unsaved StateTree's snapshot value can repeat until compile changes `compiled_signature_crc`. The benchmark therefore proves mutations through `dump_tree` and `compile` rather than through fingerprint inequality.
- Benchmark-created assets remain under `/Game/Temp/CortexMCPTest/Bench_20261103_225822` for inspection; the pre-existing `DT_TestSimple`, `ST_TestStrings`, `MM_Death_Back_01` and `TestMap` packages were restored to their original bytes (byte-compared against backups in `Saved/Issue184/`).
- Generated `.cortex/schema/` output from the benchmark was captured to `Saved/Issue184/schema-generated-benchmark-20261103_225822` and the pre-existing schema tree was restored byte-for-byte.

## Integration

Scoped commits and normal pushes/PRs in `UnrealCortex` and `cortex-toolkit`, plus the separately approved parent documentation branch for `docs/systems/cortex-material.md`. Parent gitlinks and pre-existing parent edits (`.agents/skills/**`, `.cortex/schema/**`, `.omp/config.yml`) are preserved and not committed. Issue #184 is reconciled only after both implementation PRs merge.
