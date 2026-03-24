"""Tests for explicit server registration during dual-stack migration."""

from unittest.mock import MagicMock

from cortex_mcp.capabilities import CORE_DOMAINS, minimal_router_docstrings
from cortex_mcp.tools.routers import register_router_tools
from cortex_mcp.tools.composites.blueprint import register_blueprint_compose_tools
from cortex_mcp.tools.composites.material import register_material_compose_tools
from cortex_mcp.tools.composites.widget import register_widget_compose_tools
from cortex_mcp.tools.composites.level import register_level_compose_tools
from cortex_mcp.tools.composites.scenario import register_scenario_compose_tools
from cortex_mcp.tools.standalone.editor import register_editor_standalone_tools
from cortex_mcp.tools.standalone.schema import register_schema_standalone_tools
from cortex_mcp.tools.standalone.qa import register_qa_standalone_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self, name=None, description=None, **_kwargs):
        def decorator(fn):
            self.tools[name or fn.__name__] = {"fn": fn, "description": description}
            return fn

        return decorator


def test_explicit_registration_adds_router_composite_and_standalone_tools():
    mcp = MockMCP()
    connection = MagicMock()

    register_router_tools(mcp, connection, minimal_router_docstrings())
    register_blueprint_compose_tools(mcp, connection)
    register_material_compose_tools(mcp, connection)
    register_widget_compose_tools(mcp, connection)
    register_level_compose_tools(mcp, connection)
    register_scenario_compose_tools(mcp, connection)
    register_editor_standalone_tools(mcp, connection)
    register_schema_standalone_tools(mcp, connection)
    register_qa_standalone_tools(mcp, connection)

    for domain in CORE_DOMAINS:
        assert f"{domain}_cmd" in mcp.tools

    assert "blueprint_compose" in mcp.tools
    assert "material_compose" in mcp.tools
    assert "material_instance_compose" in mcp.tools
    assert "widget_compose" in mcp.tools
    assert "level_compose" in mcp.tools
    assert "scenario_compose" in mcp.tools
    assert "qa_test_step" in mcp.tools
    assert "editor_restart" in mcp.tools
    assert "schema_generate" in mcp.tools
