# UnrealCortex MCP Server

Python MCP server that bridges Claude Code (and other MCP clients) to the UnrealCortex C++ plugin running inside Unreal Editor.

## Quick Start

```bash
cd Plugins/UnrealCortex/MCP
uv run python -m cortex_mcp
```

Port is auto-discovered from `Saved/CortexPort.txt` (written by CortexCore on editor startup). Override with `CORTEX_PORT=8742` env var.

## Directory Layout

```
MCP/
├── src/cortex_mcp/         # MCP server core
│   ├── server.py           # FastMCP server + tool discovery
│   ├── tcp_client.py       # TCP connection to CortexCore
│   ├── cache.py            # Response caching
│   └── response.py         # JSON formatting helpers
├── tools/                  # Domain tool modules (auto-discovered)
│   ├── blueprint/
│   ├── core/
│   ├── data/
│   ├── editor/
│   ├── graph/
│   ├── level/
│   ├── material/
│   ├── qa/
│   ├── reflect/
│   └── umg/
└── tests/                  # Unit + E2E tests
```

## Tool Discovery

`server.py` auto-discovers tools on startup. Rules:

1. **Scan:** Recursively scans `tools/` for `*.py` files.
2. **Skip:** Files whose name starts with `_` are skipped (`__init__.py`, `_helpers.py`, etc.).
3. **Register:** For each file, every function matching `register_*_tools` is called with `(mcp, connection)`.

### Example

`tools/blueprint/assets.py` defines:

```python
def register_blueprint_asset_tools(mcp, connection):
    @mcp.tool()
    def list_blueprints(...) -> str: ...

    @mcp.tool()
    def create_blueprint(...) -> str: ...
```

The server calls `register_blueprint_asset_tools(mcp, connection)` automatically.

### Adding a New Tool File

1. Create `tools/{domain}/my_feature.py` (no leading `_`).
2. Define `register_{name}_tools(mcp, connection)` — any name matching `register_*_tools`.
3. Decorate each tool with `@mcp.tool()`.
4. Restart the MCP server — tools are discovered on startup.

```python
# tools/mymodule/my_feature.py

def register_mymodule_feature_tools(mcp, connection):
    @mcp.tool()
    def my_tool(param: str) -> str:
        """Short description shown to LLM."""
        response = connection.send_command("mymodule.my_command", {"param": param})
        return format_response(response.get("data", {}), "my_tool")
```

> **Silent failure:** If the file is named `_my_feature.py` or the function is not named `register_*_tools`, it will never be discovered. No error is raised.

## TCP Protocol

Commands are line-delimited JSON sent to `127.0.0.1:{port}`:

```json
{"command": "data.list_datatables", "params": {}}
```

Response:

```json
{"success": true, "data": {...}}
```

Namespace prefix routes to the registered domain handler in C++. Built-in commands (`get_status`, `get_capabilities`) have no prefix.

## Testing

```bash
# Unit tests (no editor required)
cd Plugins/UnrealCortex/MCP
uv run pytest tests/ -v -k "not e2e"

# E2E tests (requires running UE editor)
uv run pytest tests/test_e2e.py -v
```

First run: `uv add --dev pytest pytest-cov`
