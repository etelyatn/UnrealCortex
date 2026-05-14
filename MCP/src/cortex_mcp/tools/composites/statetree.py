"""Explicit registration for StateTree composite tools."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from tools.statetree.composites import register_statetree_composite_tools

_STATETREE_DISAMBIG = (
    "COMPOSITE tool — use for creating or updating a full StateTree structure. "
    "For individual StateTree commands use statetree_cmd.\n\n"
)


def register_statetree_compose_tools(mcp, connection) -> None:
    """Register StateTree composition tools."""
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_statetree_composite_tools(_CaptureMCP(), connection)

    statetree_doc = _STATETREE_DISAMBIG + (captured.get("create_statetree_tree").__doc__ or "")

    @mcp.tool(name="statetree_compose", description=statetree_doc)
    def statetree_compose(
        asset_path: str,
        mode: str = "create",
        schema_class: str = "",
        root_name: str = "",
        validate: bool = True,
        compile: bool = True,
        save: bool = True,
        operations: Optional[list[dict]] = None,
        states: Optional[list[dict]] = None,
        transitions: Optional[list[dict]] = None,
        removals: Optional[dict | list[dict]] = None,
        expected_fingerprint: Optional[dict] = None,
    ) -> str:
        return captured["create_statetree_tree"](
            asset_path=asset_path,
            mode=mode,
            schema_class=schema_class,
            root_name=root_name,
            validate=validate,
            compile=compile,
            save=save,
            operations=operations,
            states=states,
            transitions=transitions,
            removals=removals,
            expected_fingerprint=expected_fingerprint,
        )
