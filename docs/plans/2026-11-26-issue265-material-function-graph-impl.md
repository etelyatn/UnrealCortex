# Issue 265 implementation: material function graphs

Design: [2026-11-26-issue265-material-function-graph-design.md](2026-11-26-issue265-material-function-graph-design.md).

## Acceptance ledger

| Request/criterion | Current evidence | Route | Minimal proof | Exit |
|---|---|---|---|---|
| `material.list_nodes` accepts a material function path | Issue report: `MATERIAL_NOT_FOUND`; source loader is `LoadMaterial` | Implement target loader + ListNodes | `Cortex.Material.Graph.Function.ListNodes` | Pass, no `MATERIAL_NOT_FOUND` |
| `list_connections` / `get_node` accept a function | Same loader in both commands | Implement | `GetNodePinsAndConnections`, live smoke | Pass |
| Graph write commands accept a function | Same loader in `add_node`, `remove_node`, `connect`, `disconnect`, `set_node_property`, `auto_layout` | Implement | `WriteAndRecompile`, `Disconnect`, `AutoLayout`, live write smoke | Pass |
| Function inputs/outputs are addressable nodes | `FunctionOutput::GetInputName` returns `NAME_None` | Reflected pin fallback `A` | `GetNodePinsAndConnections` | Pass |
| Writes recompile dependent materials | No function update path exists | `UMaterialEditingLibrary::UpdateMaterialFunction` after writes, deduplicated per batch | `StateId` changes after a write; live dependent-material recompile smoke | Pass |
| `UMaterialFunctionInstance` parameter overrides are readable, read-only | Loader refuses instances | Base-graph reads + `parameter_override` in `get_node`; writes refuse | `Instance.ReadOnlyAndOverride` | Pass |
| Material-only operations behave | Root state lives on `UMaterial` | Refuse `set_material_input` / `include_input_state` on functions | `MaterialOnlyOperationsRefused` | Pass |
| Material behavior unchanged | Existing suite | No command contract change for `UMaterial` | Existing `Cortex.Material+` | Pass |
| Save/reload persistence | No implicit save | Explicit `core.save_asset` path unchanged | Live save/reload smoke | Pass |
| Documentation | Sites below | Update systems + toolkit reference | Review | Pass |

## Files

- Create `Source/CortexMaterial/Private/Operations/CortexMaterialGraphTarget.h/.cpp`:
  `FCortexMaterialGraphTarget` (`ECortexMaterialGraphTargetKind`, load, expressions, find, root
  input, begin/end edit, recompile, read-only check).
- Modify `Source/CortexMaterial/Private/Operations/CortexMaterialGraphOps.cpp`:
  swap `LoadMaterial` for the target loader; branch the 11 commands as designed; reflected input pin
  name fallback; `asset_kind`; instance override serialization; material-only refusals; function
  recompile; unique function signature names; duplicate-rename refusal.
- Modify `Source/CortexMaterial/Private/CortexMaterialCommandHandler.cpp`: graph command param
  descriptions name materials, material functions and instances; material-only commands state their
  restriction.
- Create `Source/CortexMaterial/Private/Tests/CortexMaterialFunctionGraphTest.cpp`: the test table
  from the design.
- Create `docs/verification/2026-11-26-issue265.md`: evidence, limits, command output summary.
- Update parent `docs/systems/cortex-material.md` (workspace documentation; preserved separately
  from the plugin commit).
- Update `cortex-toolkit/resources/mcp-tool-reference.md` material graph section.

## Tasks

1. Write the failing function/MFI graph tests (red), build, run
   `Cortex.Material.Graph.Function+` and confirm `MATERIAL_NOT_FOUND`/pin failures.
2. Add `FCortexMaterialGraphTarget` and route the read commands; green the read tests.
3. Route the write commands, add pin fallback, instance refusals, and recompile; green the write
   tests.
4. Run `Cortex.Material+` (whole domain), then live smoke against the Mirror editor: read
   `/Game/LevelPrototyping/Materials/MF_ProcGrid`, create a temporary function + instance through
   `editor.run_python`, exercise reads/writes/refusals, save/reload, and clean up.
5. Update docs, bump plugin `0.4.26` -> `0.4.27` (`UnrealCortex.uplugin`, `README.md`,
   `MCP/pyproject.toml` + `uv lock`), bump toolkit patch and update its reference.
6. Independent review of the candidate diff, fix supported findings, push branches and open PRs.
   Merge is gated on explicit approval.

## Verification commands

- Build: direct `UnrealBuildTool.exe CortexSandboxEditor Win64 Development` with
  `ENGINE_PATH=D:/UnrealEngine/UE_5.8`.
- Unreal: `cli/Testing/RunTests.ps1 'Cortex.Material+'` (NullRHI).
- Python: `cd Plugins/UnrealCortex/MCP && uv run pytest tests/ -m "not e2e and not scenario and not stress" -v`
  when MCP fixtures/descriptions change; MCP code should not need changes.
- Live: plugin client against `Saved/CortexPort*.txt` for the Mirror editor.

## Behavior visible to callers

- All graph reads add `asset_kind`; function reads omit the virtual `MaterialResult` node.
- Function graphs report real `FunctionInput`/`FunctionOutput` nodes and pin `A` on outputs.
- `get_node` on a function instance can carry `parameter_override`.
- Writes on a function compile dependent materials; writes on an instance refuse
  `INVALID_OPERATION`.
- `set_material_input` and `include_input_state` stay material-only.
