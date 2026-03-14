"""Explicit registration for blueprint composite tools."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.blueprint.composites import register_blueprint_composite_tools

_BP_DISAMBIG = (
    "COMPOSITE tool — use for creating or updating a Blueprint with variables, "
    "functions, and graph logic. For individual Blueprint commands use blueprint_cmd.\n\n"
)


def register_blueprint_compose_tools(mcp, connection) -> None:
    """Register blueprint composition tools."""
    # _CaptureMCP intercepts legacy tool registration so we can re-export
    # under consolidated names without reimplementing composite logic.
    # Tech debt: remove once composites are migrated to self-contained modules.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_blueprint_composite_tools(_CaptureMCP(), connection)

    # Build description BEFORE decoration — FastMCP reads description= at decoration time
    _bp_doc = _BP_DISAMBIG + (captured.get("create_blueprint_graph").__doc__ or "")

    @mcp.tool(name="blueprint_compose", description=_bp_doc)
    def blueprint_compose(
        name: str = "",
        path: str = "",
        type: str = "Actor",
        parent_class: str = "",
        variables: Optional[list] = None,
        functions: Optional[list] = None,
        graph_name: str = "EventGraph",
        nodes: Optional[list] = None,
        connections: Optional[list] = None,
        mode: str = "create",
        asset_path: str = "",
    ) -> str:
        return captured["create_blueprint_graph"](
            name=name,
            path=path,
            type=type,
            parent_class=parent_class,
            variables=variables,
            functions=functions,
            graph_name=graph_name,
            nodes=nodes,
            connections=connections,
            mode=mode,
            asset_path=asset_path,
        )
