# UnrealCortex

**Modular MCP Plugin for AI-Powered Unreal Engine Development**

UnrealCortex turns AI into an Unreal Engine co-developer. Instead of just generating code snippets you paste in, your AI assistant can directly see what's inside the editor — DataTables, Blueprint graphs, UMG widgets, gameplay tags — and modify them in place. It understands the project the way you do.

Built on the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP), UnrealCortex bridges the gap between AI capabilities and the Unreal Editor. The AI doesn't need screenshots or exported files. It reads your project's live state, reasons about it, and makes changes — with full undo/redo support.

> **Status:** v0.1.0 Beta — Core platform and Data domain are stable. Blueprint, Graph, and UMG domains are in active development.

## The Problem

AI assistants are powerful at reasoning about code, but they're blind inside Unreal Engine. They can't see your DataTable schemas, can't inspect Blueprint node graphs, can't read widget hierarchies. Every interaction requires you to manually describe or export what's in the editor, interpret the AI's response, and apply changes by hand.

That back-and-forth is the bottleneck. The AI is fast; the human bridge between AI and editor is slow.

## The Solution

UnrealCortex removes the bridge. Your AI assistant connects directly to the running editor and operates on your project in real time:

- **"What DataTables do I have?"** — It queries the Asset Registry and returns schemas, row counts, struct types
- **"Add a Health field to the CharacterStats table"** — It modifies the row, saves the asset, and the change appears in the editor
- **"Show me the event graph of BP_Enemy"** — It reads the node graph with pin types, connections, and coordinates
- **"Create a HUD widget with a health bar and ammo counter"** — It builds the widget hierarchy, sets properties, and wires up animations
- **"Bulk import these 200 items into the loot table"** — It imports with validation, dry-run support, and upsert/replace modes

The AI operates at the same level as the editor — not through file exports, not through screenshots, but through direct structured access to Unreal's object system.

## How It Works

```
AI Assistant  ←→  MCP Server (Python)  ←→  TCP  ←→  CortexCore (C++)  ←→  Unreal Editor
                                                          ↓
                                              Domain Modules (Data, Graph, BP, UMG)
```

1. **CortexCore** runs inside the editor as a C++ plugin, providing a TCP server and generic UStruct-to-JSON serialization
2. A **Python MCP server** exposes domain-specific tools that any MCP-compatible AI assistant can call
3. Commands route through namespaced handlers (`data.query_datatable`, `graph.add_node`, `bp.create`) and execute on the Game Thread with full UObject access

Drop the plugin into any Unreal project. Open the editor. The AI can see everything.

## What AI Can Do Today

### Data Operations — CortexData

26 commands across every major data subsystem. This is where AI-powered development shines brightest — bulk data work that would take hours by hand happens in seconds through conversation.

| Subsystem | AI Capabilities |
|-----------|----------------|
| **DataTables** | Query rows with wildcard filters, inspect schemas, add/update/delete rows, bulk import with dry-run, search across all tables, full CompositeDataTable awareness |
| **GameplayTags** | Browse tag hierarchies, validate tags, register new tags with auto .ini routing |
| **DataAssets** | List by class, inspect any property via reflection, partial field updates |
| **CurveTables** | Read time/value curves, modify keys |
| **StringTables** | Browse entries, update translations across cultures |
| **Asset Search** | Find any asset by name, class, or path |

Every mutation supports undo/redo. Large responses auto-truncate with metadata so the AI knows what it's working with.

### Blueprint Development — CortexBlueprint + CortexGraph

AI can create and modify Blueprints structurally — not by generating text that you copy, but by directly operating on the Blueprint asset.

**Asset management:** Create, duplicate, delete, list Blueprints. Add variables with types, defaults, and categories. Add functions and events. Compile with detailed error feedback.

**Graph editing:** Traverse EventGraphs, function graphs, and macros. Add and remove nodes. Connect pins with type-safe validation — the AI gets clear errors on mismatches, not silent failures.

### UI Development — CortexUMG

AI can build and modify UMG interfaces by working with the widget tree directly.

**Widget tree:** Traverse hierarchies, add/remove widgets, reparent within the tree.
**Properties:** Read and write any widget property — text, colors, visibility, padding, alignment. Schema introspection tells the AI what properties are available on any widget class.
**Animations:** Create, list, and manage widget animations.

## Getting Started

### Requirements

- Unreal Engine 5.6+
- Python 3.10+ with [uv](https://docs.astral.sh/uv/)
- An MCP-compatible AI assistant ([Claude Code](https://docs.anthropic.com/en/docs/claude-code), Cursor, etc.)

### Install the Plugin

```bash
cd YourProject/Plugins
git submodule add https://github.com/etelyatn/UnrealCortex.git UnrealCortex
```

Rebuild your project. All modules load automatically in the editor.

### Set Up the MCP Server

```bash
cd Plugins/UnrealCortex/MCP
uv sync
```

Add to your `.mcp.json` (or equivalent MCP config):

```json
{
  "mcpServers": {
    "cortex_mcp": {
      "type": "stdio",
      "command": "uv",
      "args": ["run", "--directory", "Plugins/UnrealCortex/MCP", "cortex_mcp"]
    }
  }
}
```

### Connect

1. Open your project in the Unreal Editor
2. Start a conversation with your AI assistant
3. Ask it anything — "What DataTables are in this project?" is a good first test

The editor writes a port file on startup (`Saved/CortexPort.txt`), the MCP server discovers it automatically. Multiple editor instances get their own ports — no conflicts.

## Architecture

UnrealCortex is a **platform** designed to grow with AI capabilities. Each Unreal subsystem gets its own domain module that registers independently:

```cpp
void FCortexDataModule::StartupModule()
{
    auto& Registry = FModuleManager::GetModuleChecked<ICortexCoreModule>("CortexCore")
        .GetCommandRegistry();

    Registry.RegisterDomain(
        TEXT("data"), TEXT("Cortex Data"), TEXT("1.0.0"),
        MakeShared<FCortexDataCommandHandler>()
    );
}
```

**Domain isolation:** Modules depend only on CortexCore, never on each other. New domains don't risk breaking existing ones.

**Generic serialization:** CortexCore's reflection-based serializer handles any UStruct → JSON conversion. Domain modules focus on logic, not formatting.

**Game Thread safety:** All UObject operations dispatch to the Game Thread automatically. Domain authors don't think about threading.

**MCP tool discovery:** Drop Python tools into `MCP/tools/{domain}/` and they're picked up at server start. No registration boilerplate.

## What's Next

The modular architecture exists so that AI capabilities can expand domain by domain. As each module ships, AI gains deeper access to another part of Unreal Engine development:

| Domain | What AI will be able to do |
|--------|---------------------------|
| **CortexMaterial** | Tune material parameters, inspect material graphs, manage parameter collections |
| **CortexAnimation** | Work with animation montages, state machines, blend spaces |
| **CortexLevel** | Inspect and manipulate level actors, transforms, components |

The long-term vision: an AI that can work across the full breadth of Unreal Engine — data, logic, UI, materials, animation, levels — the same way a senior developer navigates between subsystems. Not replacing the developer, but eliminating the tedious translation layer between "what the AI knows how to do" and "what the editor needs."

## License

MIT

## Author

Eugene Telyatnik — [@etelyatn](https://github.com/etelyatn)
