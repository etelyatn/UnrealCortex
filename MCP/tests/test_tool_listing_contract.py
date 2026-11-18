"""Contract tests for the advertised MCP tool listing (issue #225).

The tool listing is paid by every session, so advertised descriptions must stay
consistent with the registered surface: no description may instruct a model to
call a tool that does not exist, and the composite descriptions must document
the modes the tool actually supports.
"""

import re
from unittest.mock import MagicMock

import pytest

import cortex_mcp.server as server

LEGACY_TOOL_NAMES = (
    "create_blueprint",
    "add_blueprint_variable",
    "graph_add_node",
    "create_widget_screen",
    "create_blueprint_graph",
    "create_material_graph",
    "add_material_node",
    "connect_material_nodes",
    "create_material_instance",
    "create_statetree_tree",
    "generate_project_schema",
    "level_batch",
    "get_blueprint_info",
    "blueprint_search",
    "compile_blueprint",
    "delete_blueprint",
    "duplicate_blueprint",
)


class _CaptureMCP:
    """Registration sink that records each tool name and its advertised description."""

    def __init__(self):
        self.descriptions = {}

    def tool(self, name=None, description=None, **_kwargs):
        def decorator(fn):
            self.descriptions[name or fn.__name__] = (
                description if description is not None else (fn.__doc__ or "")
            )
            return fn

        return decorator


@pytest.fixture()
def default_surface(monkeypatch):
    """Register the default tool surface offline against a minimal capabilities cache."""
    mcp = _CaptureMCP()
    monkeypatch.setattr(
        server, "load_capabilities_cache", lambda: {"domains": {"data": {"commands": []}}}
    )
    connection = MagicMock()
    server._register_explicit_tools(mcp, connection)
    return mcp.descriptions


def test_no_advertised_description_names_an_unregistered_tool(default_surface):
    registered = set(default_surface)
    assert registered, "the default surface must register tools"
    for tool_name, description in default_surface.items():
        for legacy in LEGACY_TOOL_NAMES:
            assert legacy not in registered or legacy == tool_name
            pattern = rf"\b{re.escape(legacy)}\b"
            assert not re.search(pattern, description), (
                f"description of '{tool_name}' names unregistered tool '{legacy}'"
            )


def test_blueprint_compose_description_documents_both_modes(default_surface):
    description = default_surface["blueprint_compose"]
    assert "mode='create'" in description
    assert "mode='update'" in description
    # The legacy create-only helper text contradicted the update mode; it must be gone.
    assert 'Only "create"' not in description
    assert "is refused with migration guidance" not in description


@pytest.mark.parametrize(
    ("tool_name", "bound"),
    [
        ("blueprint_compose", 4200),
        ("material_compose", 4200),
        ("widget_compose", 3100),
    ],
)
def test_composite_descriptions_stay_bounded(default_surface, tool_name, bound):
    assert len(default_surface[tool_name]) <= bound, (
        f"{tool_name} description is {len(default_surface[tool_name])} chars; bound {bound}"
    )


def test_default_surface_has_no_duplicate_reflect_wrappers():
    from cortex_mcp.capabilities import CORE_DOMAINS, resolve_toolset

    surface = resolve_toolset("all", CORE_DOMAINS)
    assert surface is None  # no filter means the registered surface itself must exclude them
    from cortex_mcp.capabilities import _TOOLSETS

    for preset in _TOOLSETS.values():
        assert not (preset & {
            "get_dependencies", "get_referencers", "query_class_detail",
            "query_class_hierarchy", "query_overrides", "query_usages",
        })


def test_default_surface_registers_no_duplicate_reflect_wrappers(default_surface):
    for removed in (
        "get_dependencies", "get_referencers", "query_class_detail",
        "query_class_hierarchy", "query_overrides", "query_usages",
    ):
        assert removed not in default_surface, f"{removed} must not be registered"
    for kept in (
        "scan_project", "reflect_cache_status", "rebuild_graph_cache",
        "query_class_context", "impact_analysis",
    ):
        assert kept in default_surface, f"{kept} must stay registered"
    assert "reflect_cmd" in default_surface
