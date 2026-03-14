"""Tests that MCP-registered composite wrappers expose full schema documentation."""
import importlib
import sys
from pathlib import Path
from unittest.mock import MagicMock
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
