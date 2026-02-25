# UnrealCortex

<p align="center">
  <img src="https://img.shields.io/badge/Unreal%20Engine-5.6%2B-blue?style=flat-square&logo=unrealengine" alt="UE 5.6+">
  <img src="https://img.shields.io/badge/Type-Editor%20Only-green?style=flat-square" alt="Editor Only">
  <img src="https://img.shields.io/badge/Modules-10-lightgrey?style=flat-square" alt="10 Modules">
  <img src="https://img.shields.io/badge/Python-3.10%2B-yellow?style=flat-square&logo=python" alt="Python 3.10+">
  <img src="https://img.shields.io/badge/License-MIT-lightgrey?style=flat-square" alt="MIT">
</p>

**Give your AI hands inside Unreal Engine.**

Your AI assistant can already write code. UnrealCortex lets it work *inside* the editor — querying DataTables, editing Blueprint graphs, building UMG hierarchies, placing actors, and even playing and testing your game autonomously. No copy-pasting, no file exports. Changes appear live with full undo support.

All UObject access dispatches to the Game Thread via `AsyncTask(ENamedThreads::GameThread)`. All mutations are wrapped in `FScopedTransaction` — standard Ctrl+Z undo works. Zero runtime footprint: all 10 modules declare `Type: Editor`, load at `PostEngineInit`, and are stripped from shipping builds.

> **Status:** v0.1.0 Beta — All 10 domain modules shipped and tested.

---

## Architecture

```mermaid
flowchart LR
    AI["AI Assistant"]
    MCP["Python MCP Server"]
    Core["CortexCore\nC++ Plugin"]
    UE["Unreal Editor"]

    AI <-- "MCP tools" --> MCP
    MCP <-- "JSON over TCP" --> Core
    Core <-- "Game Thread\nUObject access" --> UE
```

Commands are namespaced: `{domain}.{command}` — e.g. `data.query_datatable`, `bp.create`, `graph.add_node`. CortexCore routes each command to its registered domain handler and dispatches to the Game Thread. The port is auto-discovered via `Saved/CortexPort.txt` — multiple editor instances each get their own port.

---

## Quick Start

### Requirements

- Unreal Engine 5.6+
- Python 3.10+ with [uv](https://docs.astral.sh/uv/) (`pip install uv` or see [uv docs](https://docs.astral.sh/uv/getting-started/installation/))
- An MCP-compatible AI assistant ([Claude Code](https://docs.anthropic.com/en/docs/claude-code), Cursor, etc.)

### Step 1 — Install the Plugin

```bash
cd YourProject/Plugins
git submodule add https://github.com/etelyatn/UnrealCortex.git UnrealCortex
```

Add the plugin to your `.uproject`:

```json
{
  "Plugins": [
    { "Name": "UnrealCortex", "Enabled": true }
  ]
}
```

Rebuild your project. All 10 modules load automatically at `PostEngineInit` — after `IAssetRegistry` and the Blueprint compilation system are ready. All modules are `Type: Editor` and are stripped from shipping builds.

### Step 2 — Set Up the MCP Server

Run once to install Python dependencies:

```bash
cd Plugins/UnrealCortex/MCP
uv sync
```

Add to your `.mcp.json` (relative to your project root — always open your AI assistant from the project directory):

```json
{
  "mcpServers": {
    "cortex_mcp": {
      "command": "uv",
      "args": ["run", "--directory", "Plugins/UnrealCortex/MCP", "cortex-mcp"]
    }
  }
}
```

Open your project in the Unreal Editor. CortexCore writes `Saved/CortexPort.txt` on startup — the MCP server discovers it automatically. If the editor is not open, MCP tool calls will return an `EDITOR_NOT_RUNNING` error. If you restart the editor, the MCP server picks up the new port file automatically.

### Step 3 — Install Cortex Toolkit

> [!NOTE]
> UnrealCortex provides the raw MCP tools. **[Cortex Toolkit](https://github.com/etelyatn/cortex-toolkit)** adds domain-specific skills, specialist agents, and project memory so the AI knows *when* and *how* to use them. Recommended for all workflows.

**Option A — Claude Code marketplace:**

Run this slash command inside Claude Code to add the marketplace, then browse and install only the domains you need:
```
/plugin marketplace add etelyatn/cortex-toolkit
```

**Option B — Install selectively from your terminal:**

```bash
claude plugin add etelyatn/cortex-toolkit/cortex-core      # Required
claude plugin add etelyatn/cortex-toolkit/cortex-data       # Pick your domains
claude plugin add etelyatn/cortex-toolkit/cortex-blueprint
claude plugin add etelyatn/cortex-toolkit/cortex-ui
claude plugin add etelyatn/cortex-toolkit/cortex-material
claude plugin add etelyatn/cortex-toolkit/cortex-level
claude plugin add etelyatn/cortex-toolkit/cortex-qa
claude plugin add etelyatn/cortex-toolkit/cortex-reflect
```

Then run `/cortex-start` inside Claude Code — it checks your setup, verifies the editor connection, and walks you through your first task. Run once to initialize your project; use `/cortex-help` during any session for contextual suggestions. If the connection check fails, confirm the Unreal Editor is open with the plugin loaded.

---

## Project Memory

> [!IMPORTANT]
> Cortex Toolkit creates a `.cortex/` directory in your project root. Fill it with your project's conventions — table schemas, Blueprint naming rules, material hierarchies, screen inventory. Instead of explaining your conventions in every chat, write them once and every agent respects them automatically.

The session-start hook injects `context.md` automatically. Domain agents read their specific file (e.g., `domains/blueprints.md`) before every task.

```
.cortex/
├── config.yaml          ← engine path, active domains
├── context.md           ← shared project knowledge (read every session)
└── domains/
    ├── data.md          ← table schemas, balance rules
    ├── blueprints.md    ← class hierarchy, conventions
    ├── material.md      ← material conventions, instance hierarchies
    ├── umg.md           ← screen inventory, style guide
    ├── level.md         ← actor conventions, level structure
    ├── qa.md            ← test scenarios, assertion patterns
    └── reflect.md       ← class hierarchy notes, scan scope
```

---

## What AI Can Do

<details>
<summary><strong>Data — CortexData</strong> &nbsp;·&nbsp; 26 commands &nbsp;·&nbsp; DataTables, GameplayTags, DataAssets, CurveTables, StringTables</summary>

<br>

| Subsystem | Capabilities |
|-----------|-------------|
| **DataTables** | Query rows with wildcard filters, inspect schemas, add/update/delete rows, bulk import with dry-run, search across all tables, full CompositeDataTable awareness |
| **GameplayTags** | Browse tag hierarchies, validate tags, register new tags with auto `.ini` routing |
| **DataAssets** | List by class, inspect any property via reflection, partial field updates |
| **CurveTables** | Read time/value curves, modify keys |
| **StringTables** | Browse entries, update translations |
| **Asset Search** | Find any asset by name, class, or path |

Every mutation wrapped in `FScopedTransaction`. Large responses auto-truncate with metadata so the AI knows what it's working with.

</details>

<details>
<summary><strong>Blueprint — CortexBlueprint + CortexGraph</strong> &nbsp;·&nbsp; Create, modify, and compile Blueprints structurally</summary>

<br>

**Asset management:** Create, duplicate, delete, list Blueprints. Add variables with types, defaults, and categories. Add functions and events. Compile with error feedback via `FKismetCompilerContext`.

**Graph editing:** Traverse EventGraphs, function graphs, and macros via `UEdGraph`. Add and remove nodes. Connect pins with type-safe validation — mismatches return clear errors, not silent failures.

</details>

<details>
<summary><strong>UI — CortexUMG</strong> &nbsp;·&nbsp; Build and modify UMG widget hierarchies</summary>

<br>

**Widget tree:** Traverse hierarchies, add/remove widgets, reparent within the tree.

**Properties:** Read and write any widget property — text, colors, visibility, padding, alignment. Schema introspection returns available properties for any widget class.

**Animations:** Create, list, and manage widget animations.

</details>

<details>
<summary><strong>Materials — CortexMaterial</strong> &nbsp;·&nbsp; Inspect and modify materials, instances, and parameter collections</summary>

<br>

**Asset management:** List, create, and delete materials and material instances. Duplicate existing materials as starting points.

**Parameters:** Read and write scalar, vector, and texture parameters on instances. Bulk-set multiple parameters in one call. Reset to parent defaults.

**Material graphs:** List expression nodes, inspect connections, add/remove nodes, wire them together.

**Parameter collections:** Create and manage material parameter collections — add, remove, and set collection parameters.

</details>

<details>
<summary><strong>Level Design — CortexLevel</strong> &nbsp;·&nbsp; Actor placement, transforms, components, organization, streaming</summary>

<br>

**Actor lifecycle:** Spawn actors by class, delete, duplicate, rename. Set transforms (position, rotation, scale). Query bounding boxes.

**Discovery:** List all actors, find by class, tag, or name. Select actors programmatically.

**Components:** Add, remove, list components. Get and set any component property via reflection.

**Organization:** Assign actors to folders, set gameplay tags, group/ungroup, attach/detach hierarchies.

**Streaming:** List, load, and unload sublevels. Control sublevel visibility. Manage data layers.

**Batch:** `level_batch` composite command for multi-step scene construction in a single call.

</details>

<details>
<summary><strong>Editor Control — CortexEditor</strong> &nbsp;·&nbsp; PIE lifecycle, viewport, input injection</summary>

<br>

**PIE lifecycle:** Start, stop, pause, resume, and restart Play-In-Editor sessions.

**Viewport:** Get and set camera position/rotation, capture screenshots, switch render modes.

**Input injection:** Press keys, run multi-step input sequences with configurable timing.

**Editor management:** Execute console commands. Adjust time dilation. Shutdown/restart editor.

**Diagnostics:** Query world info, viewport state, and recent log output.

</details>

<details>
<summary><strong>Gameplay QA — CortexQA</strong> &nbsp;·&nbsp; Play-test your game through structured scenarios</summary>

<br>

**World queries:** List actors with details, inspect specific actors and the player character at runtime.

**Game actions:** Move the player to locations, interact with objects, look at targets, teleport.

**Assertions:** Assert game state conditions, wait for conditions with timeout, record named test steps.

**Scenario engine:** `run_scenario_inline` executes multi-step test sequences — move here, interact with that, verify this state — in a single declarative call.

**Reproducibility:** Set random seed for deterministic test runs.

</details>

<details>
<summary><strong>Project Analysis — CortexReflect</strong> &nbsp;·&nbsp; Class hierarchy, properties, cross-references</summary>

<br>

**Project scanning:** Build a class hierarchy cache from loaded C++ modules and Blueprint assets.

**Class queries:** Query any class for its properties, functions, metadata, and parent chain. Full context (self + parent + children) in one call.

**Hierarchy navigation:** Walk the class tree across C++ and Blueprint boundaries.

**Override detection:** Find which Blueprint classes override specific C++ base functions.

**Usage search:** Search for references to a class across all loaded Blueprint assets.

</details>

---

## Architecture Deep Dive

<details>
<summary>Threading · Undo/Redo · Extensibility · Cook Safety</summary>

<br>

### Threading Model

The TCP server runs on a dedicated `FRunnable` thread and shuts down cleanly in `ShutdownModule()`. All UObject access dispatches to the Game Thread:

```cpp
AsyncTask(ENamedThreads::GameThread, [Command, Params, Callback]()
{
    FCortexCommandResult Result = Handler->Execute(Command, Params, Callback);
});
```

Domain module authors do not manage threading. CortexCore handles dispatch before invoking any `ICortexDomainHandler`.

### Undo / Redo

Every write operation is wrapped in `FScopedTransaction`:

```cpp
FScopedTransaction Transaction(
    FText::FromString(FString::Printf(TEXT("Cortex: %s"), *CommandName))
);
// ... mutation ...
```

Standard Ctrl+Z / Ctrl+Y undo works on all AI-driven changes.

### Extending with a Custom Domain

Implement `ICortexDomainHandler` and register at module startup:

```cpp
// The interface contract
virtual FCortexCommandResult Execute(
    const FString& Command,
    const TSharedPtr<FJsonObject>& Params,
    FDeferredResponseCallback DeferredCallback = nullptr  // optional: for async responses
) = 0;

virtual TArray<FCortexCommandInfo> GetSupportedCommands() const = 0;  // drives get_capabilities
```

```cpp
void FMyDomainModule::StartupModule()
{
    auto& Registry = FModuleManager::GetModuleChecked<ICortexCoreModule>("CortexCore")
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("mydomain"), TEXT("My Domain"), TEXT("1.0.0"),
        MakeShared<FMyDomainCommandHandler>()
    );
}
```

`GetSupportedCommands()` feeds the `get_capabilities` built-in command — the AI can discover what your domain exposes automatically. Drop Python tools into `MCP/tools/mydomain/` and they are discovered at server start with no registration boilerplate.

### Module Dependencies

| Module | Depends On | Key UE Engine Modules |
|--------|-----------|----------------------|
| **CortexCore** | — | `Sockets` · `Networking` · `Json` · `JsonUtilities` · `GameplayTags` · `UnrealEd` · `AssetRegistry` |
| **CortexGraph** | CortexCore | `BlueprintGraph` · `KismetCompiler` · `UnrealEd` |
| **CortexBlueprint** | CortexCore · CortexGraph | `BlueprintGraph` · `Kismet` · `KismetCompiler` · `AssetRegistry` · `GameplayTags` |
| **CortexMaterial** | CortexCore · CortexGraph | `MaterialEditor` · `AssetRegistry` |
| **CortexData** | CortexCore | `GameplayTags` · `AssetRegistry` · `UnrealEd` |
| **CortexEditor** | CortexCore | `LevelEditor` · `Slate` · `SlateCore` · `EnhancedInput` · `ImageWrapper` · `RenderCore` |
| **CortexQA** | CortexCore · CortexEditor | `NavigationSystem` · `AIModule` · `GameplayTags` |
| **CortexLevel** | CortexCore | `LevelEditor` · `DataLayerEditor` |
| **CortexUMG** | CortexCore | `UMG` · `UMGEditor` · `Slate` · `SlateCore` · `MovieScene` |
| **CortexReflect** | CortexCore | `AssetRegistry` · `BlueprintGraph` · `Kismet` |

Domain modules depend only on CortexCore (and shared infrastructure: CortexGraph, CortexEditor). Never on each other.

### Cook and Packaging Safety

All 10 modules declare `"Type": "Editor"` in `UnrealCortex.uplugin`. Because `Type: Editor` modules are not loaded in non-editor targets (cook, server, game), the `PostEngineInit` load phase is only relevant in the editor. The plugin is never included in cooked or packaged builds.

### Generic Serialization

`FCortexSerializer` uses Unreal's property system (`FProperty*`, `UStruct*`) to convert any UStruct to JSON. Domain modules focus on logic, not formatting — they never write JSON manually.

### Hot Reload

After a Live Coding recompile, the TCP server restarts automatically with the new module. If reconnection fails, restarting the editor restores the connection.

</details>

---

## Known Limitations

These are the current boundaries of the beta. They're on the roadmap but not yet resolved:

- **Widget animation tracks:** Named animations can be created and listed, but property track binding and keyframe creation are not yet implemented
- **Blueprint compile diagnostics:** `bp.compile` returns success/failure but error locations are unstructured text, not node-level diagnostics
- **Concurrent input sequences:** `run_input_sequence` works correctly for single-agent use; concurrent sequences from multiple agents may route callbacks incorrectly
- **Reflect coverage:** `query_class_hierarchy` and `find_usages` only see Blueprint classes loaded in memory; unloaded Blueprints are silently skipped
- **Batch pipeline:** No transactional rollback; if a batch step fails, completed steps are not undone

---

## What's Next

| Domain | What AI will be able to do |
|--------|---------------------------|
| **CortexAnimation** | Work with animation montages, state machines, blend spaces |
| **CortexNiagara** | Modify particle system parameters, emitter configuration |
| **Enhanced migration tooling** | Multi-Blueprint orchestrated Blueprint-to-C++ migration workflows |

---

## License

MIT

## Author

Eugene Telyatnik — [@etelyatn](https://github.com/etelyatn)
