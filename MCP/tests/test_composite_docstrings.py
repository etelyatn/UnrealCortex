"""Tests that MCP-registered composite wrappers expose full schema documentation."""
import importlib
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch
import pytest

# Ensure src is on path (mirrors how the server loads tools)
_MCP_SRC = Path(__file__).resolve().parents[1] / "src"
if str(_MCP_SRC) not in sys.path:
    sys.path.insert(0, str(_MCP_SRC))


class _MockMCP:
    """Captures the description= kwarg FastMCP receives at decoration time.

    FastMCP reads description= (not fn.__doc__) when tools are registered.
    This mock must mirror that behavior so tests catch the real failure mode.
    """
    def __init__(self):
        self._descriptions: dict[str, str] = {}

    def tool(self, name=None, description=None, **_kwargs):
        def decorator(fn):
            key = name or fn.__name__
            # FastMCP uses description= kwarg, NOT fn.__doc__ — mirror that here
            self._descriptions[key] = description or ""
            return fn
        return decorator


def _register(module_path: str, func_name: str) -> dict[str, str]:
    """Returns {tool_name: description_string} exactly as FastMCP would see it."""
    mod = importlib.import_module(module_path)
    mcp = _MockMCP()
    conn = MagicMock()
    getattr(mod, func_name)(mcp, conn)
    return mcp._descriptions


# --- material_compose ---

class TestMaterialComposeDocstring:
    @pytest.fixture(scope="class")
    def tools(self):
        # Returns {tool_name: description_string} as FastMCP sees it
        return _register(
            "cortex_mcp.tools.composites.material",
            "register_material_compose_tools",
        )

    def test_has_disambiguation_header(self, tools):
        doc = tools.get("material_compose", "")
        assert "COMPOSITE" in doc, "Must start with COMPOSITE disambiguation line"

    def test_references_material_cmd(self, tools):
        doc = tools.get("material_compose", "")
        assert "material_cmd" in doc, "Must cross-reference the router tool"

    def test_documents_connection_format(self, tools):
        doc = tools.get("material_compose", "")
        # Check for literal '"from"' — not just any word containing 'from'
        assert '"from"' in doc or "NodeName.PinName" in doc, \
            "Must document the 'from'/'to' dot-notation connection format"

    def test_documents_node_class_key(self, tools):
        doc = tools.get("material_compose", "")
        assert '"class"' in doc or "'class'" in doc, \
            "Must document that nodes use 'class' key (not 'type')"

    def test_not_one_liner(self, tools):
        doc = tools.get("material_compose", "")
        assert len(doc) > 200, "Docstring must be the full schema doc, not a one-liner"


# --- blueprint_compose ---

class TestBlueprintComposeDocstring:
    @pytest.fixture(scope="class")
    def tools(self):
        return _register(
            "cortex_mcp.tools.composites.blueprint",
            "register_blueprint_compose_tools",
        )

    def test_has_disambiguation_header(self, tools):
        doc = tools.get("blueprint_compose", "")
        assert "COMPOSITE" in doc

    def test_references_blueprint_cmd(self, tools):
        doc = tools.get("blueprint_compose", "")
        assert "blueprint_cmd" in doc

    def test_not_one_liner(self, tools):
        doc = tools.get("blueprint_compose", "")
        assert len(doc) > 200


# --- widget_compose ---

class TestWidgetComposeDocstring:
    @pytest.fixture(scope="class")
    def tools(self):
        return _register(
            "cortex_mcp.tools.composites.widget",
            "register_widget_compose_tools",
        )

    def test_has_disambiguation_header(self, tools):
        doc = tools.get("widget_compose", "")
        assert "COMPOSITE" in doc

    def test_references_umg_cmd(self, tools):
        doc = tools.get("widget_compose", "")
        assert "umg_cmd" in doc

    def test_not_one_liner(self, tools):
        doc = tools.get("widget_compose", "")
        assert len(doc) > 200


# --- level_compose ---

class TestLevelComposeDocstring:
    @pytest.fixture(scope="class")
    def tools(self):
        return _register(
            "cortex_mcp.tools.composites.level",
            "register_level_compose_tools",
        )

    def test_has_disambiguation_header(self, tools):
        doc = tools.get("level_compose", "")
        assert "COMPOSITE" in doc

    def test_references_level_cmd(self, tools):
        doc = tools.get("level_compose", "")
        assert "level_cmd" in doc

    def test_not_one_liner(self, tools):
        doc = tools.get("level_compose", "")
        assert len(doc) > 200


# --- router disambiguation hints ---

# Minimal fake capabilities that mirrors the real schema structure.
# commands must be a list of dicts (each with at least a "name" key),
# matching what _format_command_signature expects.
_FAKE_CAPS = {
    "domains": {
        "material": {
            "commands": [
                {"name": "create_material_graph", "params": []},
                {"name": "set_material_property", "params": []},
            ]
        },
        "blueprint": {
            "commands": [
                {"name": "create_blueprint", "params": []},
                {"name": "compile_blueprint", "params": []},
            ]
        },
        "umg": {
            "commands": [
                {"name": "get_widget_tree", "params": []},
                {"name": "set_widget_property", "params": []},
            ]
        },
        "level": {
            "commands": [
                {"name": "spawn_actor", "params": []},
                {"name": "get_actors", "params": []},
            ]
        },
    }
}


class TestRouterDisambiguation:
    """Router docstrings must mention composite alternatives for creation tasks."""

    def _build(self) -> dict[str, str]:
        from cortex_mcp.capabilities import build_router_docstrings
        # Patch load_capabilities_cache so tests never need a live editor or schema file
        with patch("cortex_mcp.capabilities.load_capabilities_cache", return_value=_FAKE_CAPS):
            return build_router_docstrings(_FAKE_CAPS)

    def test_material_cmd_mentions_material_compose(self):
        doc = self._build().get("material", "")
        assert "material_compose" in doc, \
            "material_cmd docstring must reference material_compose for graph creation"

    def test_blueprint_cmd_mentions_blueprint_compose(self):
        doc = self._build().get("blueprint", "")
        assert "blueprint_compose" in doc

    def test_umg_cmd_mentions_widget_compose(self):
        doc = self._build().get("umg", "")
        assert "widget_compose" in doc

    def test_level_cmd_mentions_level_compose(self):
        doc = self._build().get("level", "")
        assert "level_compose" in doc
