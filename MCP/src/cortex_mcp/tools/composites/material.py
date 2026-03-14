"""Explicit registration for material composite tools."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.material.composites import register_material_composite_tools

_MAT_DISAMBIG = (
    "COMPOSITE tool — use for creating a full material graph from scratch. "
    "For individual material commands use material_cmd.\n\n"
)
_MAT_INST_DISAMBIG = (
    "COMPOSITE tool — use for creating a material instance with parameter overrides. "
    "For individual material commands use material_cmd.\n\n"
)


def register_material_compose_tools(mcp, connection) -> None:
    """Register material composition tools."""
    # _CaptureMCP: intercept legacy registration, re-export under consolidated names.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_material_composite_tools(_CaptureMCP(), connection)

    # Build descriptions BEFORE decoration — FastMCP reads description= at decoration time
    _mat_doc = _MAT_DISAMBIG + (captured.get("create_material_graph").__doc__ or "")
    _mat_inst_doc = _MAT_INST_DISAMBIG + (captured.get("create_material_instance").__doc__ or "")

    @mcp.tool(name="material_compose", description=_mat_doc)
    def material_compose(
        name: str,
        path: str,
        nodes: list,
        connections: list,
        material_properties: Optional[dict] = None,
        instances: Optional[list] = None,
    ) -> str:
        return captured["create_material_graph"](
            name=name,
            path=path,
            nodes=nodes,
            connections=connections,
            material_properties=material_properties,
            instances=instances,
        )

    @mcp.tool(name="material_instance_compose", description=_mat_inst_doc)
    def material_instance_compose(
        name: str,
        path: str,
        parent: str,
        parameters: Optional[list] = None,
    ) -> str:
        return captured["create_material_instance"](
            name=name,
            path=path,
            parent=parent,
            parameters=parameters,
        )
