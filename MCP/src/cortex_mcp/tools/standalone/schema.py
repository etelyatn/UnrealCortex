"""Explicit registration for schema generation tools."""

from __future__ import annotations

import sys
from pathlib import Path

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.core.schema import register_schema_tools


def register_schema_standalone_tools(mcp, connection) -> None:
    """Register schema generation/status tools."""
    # _CaptureMCP: intercept legacy registration, re-export under consolidated names.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_schema_tools(_CaptureMCP(), connection)

    @mcp.tool(
        name="schema_generate",
        description=(
            "Generate LLM-readable `.cortex/schema/` files (DataTable catalog, struct schemas, "
            "GameplayTag prefixes, DataAsset inventories, example rows) for a domain and return the "
            "generated file paths plus any errors. Requires a running Unreal Editor with the "
            "UnrealCortex plugin.\n"
        ),
    )
    def schema_generate(domain: str = "all") -> str:
        return captured["generate_project_schema"](domain=domain)
