"""Unit tests for reflect tool registration and response formatting."""

import copy
import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

# Add tools/ to sys.path matching existing test patterns
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))


class TestReflectToolRegistration:
    """Test that reflect tools register correctly."""

    def test_hierarchy_registration(self):
        from reflect.hierarchy import register_reflect_hierarchy_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_hierarchy_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_detail_registration(self):
        from reflect.detail import register_reflect_detail_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_detail_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_overrides_registration(self):
        from reflect.overrides import register_reflect_override_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_override_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_usages_registration(self):
        from reflect.usages import register_reflect_usage_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_usage_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_cache_registration(self):
        from reflect.cache import register_reflect_cache_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_cache_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_context_registration(self):
        from reflect.context import register_reflect_context_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_context_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_init_registers_all(self):
        from reflect import register_reflect_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_tools(mcp, connection)
        assert mcp.tool.call_count >= 6


class TestHierarchyFlattening:
    """Test the tree-to-flat-list conversion."""

    def test_flatten_simple_tree(self):
        from reflect.hierarchy import _flatten_hierarchy

        tree = {
            "name": "AActor",
            "type": "cpp",
            "module": "Engine",
            "children": [
                {
                    "name": "AMyActor",
                    "type": "cpp",
                    "module": "MyGame",
                    "source_path": "MyActor.h",
                    "children": [],
                }
            ],
            "total_classes": 2,
            "cpp_count": 2,
            "blueprint_count": 0,
        }

        result = _flatten_hierarchy(tree)
        assert "classes" in result
        assert len(result["classes"]) == 2
        assert result["classes"][0]["name"] == "AActor"
        assert result["classes"][0]["depth"] == 0
        assert result["classes"][0]["parent"] is None
        assert result["classes"][1]["name"] == "AMyActor"
        assert result["classes"][1]["depth"] == 1
        assert result["classes"][1]["parent"] == "AActor"

    def test_flatten_preserves_counts(self):
        from reflect.hierarchy import _flatten_hierarchy

        tree = {
            "name": "AActor",
            "type": "cpp",
            "children": [],
            "total_classes": 1,
            "cpp_count": 1,
            "blueprint_count": 0,
        }

        result = _flatten_hierarchy(tree)
        assert result["total_classes"] == 1
        assert result["cpp_count"] == 1
        assert result["blueprint_count"] == 0

    def test_flatten_empty_name_defensive(self):
        from reflect.hierarchy import _flatten_hierarchy

        tree = {"children": [], "total_classes": 1, "cpp_count": 1, "blueprint_count": 0}
        result = _flatten_hierarchy(tree)
        assert result["classes"][0]["name"] == "Unknown"

    def test_flatten_uses_children_and_never_duplicates_native_flat_classes(self):
        from reflect.hierarchy import _flatten_hierarchy

        # A native response that also carries an opt-in flat duplicate must not be emitted twice:
        # the collector walks children, so the framed view stays navigable and deduplicated.
        tree = {
            "name": "AActor",
            "type": "cpp",
            "classes": ["AActor", "AMyActor"],
            "children": [{"name": "AMyActor", "type": "cpp", "children": []}],
            "total_classes": 2,
            "cpp_count": 2,
            "blueprint_count": 0,
        }

        result = _flatten_hierarchy(tree)

        assert [row["name"] for row in result["classes"]] == ["AActor", "AMyActor"]
        assert result["total_classes"] == 2


class TestDetailLevels:
    """Test detail level pruning."""

    def test_summary_level(self):
        from reflect.detail import _prune_detail

        full_data = {
            "name": "AMyActor",
            "type": "cpp",
            "parent": "AActor",
            "module": "MyGame",
            "source_path": "MyActor.h",
            "properties": [{"name": "Health", "type": "float"}],
            "functions": [{"name": "TakeDamage", "return_type": "float"}],
            "components": [],
            "interfaces": [],
            "blueprint_children_count": 2,
        }

        result = _prune_detail(full_data, "summary")
        assert "name" in result
        assert "type" in result
        assert "parent" in result
        assert "source_path" in result  # included at summary level
        assert "properties" not in result
        assert "functions" not in result

    def test_properties_level(self):
        from reflect.detail import _prune_detail

        full_data = {
            "name": "AMyActor",
            "type": "cpp",
            "parent": "AActor",
            "module": "MyGame",
            "properties": [{"name": "Health", "type": "float"}],
            "functions": [{"name": "TakeDamage", "return_type": "float"}],
            "components": [{"name": "Mesh", "type": "UStaticMeshComponent"}],
            "interfaces": [],
            "blueprint_children_count": 2,
        }

        result = _prune_detail(full_data, "properties")
        assert "properties" in result
        assert "components" in result  # included at properties level
        assert "interfaces" in result
        assert "functions" not in result

    def test_full_level(self):
        from reflect.detail import _prune_detail

        full_data = {
            "name": "AMyActor",
            "type": "cpp",
            "parent": "AActor",
            "module": "MyGame",
            "source_path": "MyActor.h",
            "properties": [{"name": "Health", "type": "float"}],
            "functions": [{"name": "TakeDamage", "return_type": "float"}],
            "components": [],
            "interfaces": [],
            "blueprint_children_count": 2,
        }

        result = _prune_detail(full_data, "full")
        assert "properties" in result
        assert "functions" in result
        assert "components" in result


# ---------------------------------------------------------------------------
# Task 12 (issue 180, TE11): native summary before extraction, honest bounds
# ---------------------------------------------------------------------------
#
# These regressions describe the Python boundary only. The forwarding assertions below prove
# *wiring* -- which native request the reader issues -- and are never evidence that the native
# side honoured the requested detail level. The task brief requires the connected check
# (query_class_detail / query_class_context against a running editor through the registered tool
# collection) for that; re-pinned mock arguments are not accepted as that evidence.

_FULL_DETAIL = {
    "name": "AMyActor",
    "type": "cpp",
    "parent": "AActor",
    "module": "MyGame",
    "source_path": "MyActor.h",
    "interfaces": [],
    "properties": [{"name": "Health", "type": "float"}],
    "functions": [{"name": "TakeDamage", "return_type": "float"}],
    "components": [{"name": "Mesh", "type": "UStaticMeshComponent"}],
    "blueprint_children_count": 2,
}

# Native detail='summary' coverage: identity/parent/routing only, no declared members.
_SUMMARY_DETAIL = {
    "name": "AActor",
    "type": "cpp",
    "parent": "UObject",
    "module": "Engine",
    "source_path": "Actor.h",
    "interfaces": [],
    "blueprint_children_count": 3,
}


class MockMCP:
    """Minimal FastMCP stand-in that captures the registered tool functions by name."""

    def __init__(self):
        self.tools = {}

    def tool(self, name=None, description=None, **_kwargs):
        def decorator(func):
            self.tools[name or func.__name__] = func
            return func

        return decorator


class StubReflectConnection:
    """Fake UEConnection serving native-shaped payloads and recording every request."""

    def __init__(self, handler):
        self._handler = handler
        self.requests = []

    def send_command_cached(self, command, params=None, ttl=None):
        request = dict(params or {})
        self.requests.append((command, request))
        return {"success": True, "data": copy.deepcopy(self._handler(command, request))}

    def detail_requests(self):
        """The reflect.class_detail requests this reader issued, in order."""
        return [params for command, params in self.requests if command == "reflect.class_detail"]


def _detail_handler(command, params):
    """Mirror the native class_detail contract: 'summary' skips member extraction."""
    if command != "reflect.class_detail":
        raise AssertionError(f"unexpected native command: {command}")
    if params.get("detail") == "summary":
        return _SUMMARY_DETAIL
    return _FULL_DETAIL


def _capped_hierarchy(params):
    """A hierarchy page that reached its node cap, mirroring the native bound."""
    limit = int(params.get("max_results", 100))
    return {
        "name": params.get("root", "Unknown"),
        "type": "cpp",
        "children": [{"name": f"Child_{i}", "type": "cpp"} for i in range(max(limit - 1, 0))],
        "total_classes": limit,
        "cpp_count": limit,
        "blueprint_count": 0,
        "truncated": True,
    }


def _context_handler(command, params):
    if command == "reflect.class_detail":
        # The parent is only reachable through its native summary; the requested class keeps the
        # full extraction that the retained properties default needs.
        if params.get("class_name") == "AActor":
            return _SUMMARY_DETAIL
        return _FULL_DETAIL
    if command == "reflect.class_hierarchy":
        return _capped_hierarchy(params)
    raise AssertionError(f"unexpected native command: {command}")


class TestNativeSummaryBeforeExtraction:
    """query_class_detail asks the native side for the declared view before extraction."""

    @staticmethod
    def _tool(handler):
        from reflect.detail import register_reflect_detail_tools

        mcp = MockMCP()
        connection = StubReflectConnection(handler)
        register_reflect_detail_tools(mcp, connection)
        return mcp.tools["query_class_detail"], connection

    def test_default_detail_targets_native_summary(self):
        tool, connection = self._tool(_detail_handler)

        payload = json.loads(tool(class_name="AActor"))

        assert [params.get("detail") for params in connection.detail_requests()] == ["summary"]
        assert payload["name"] == "AActor"
        assert "properties" not in payload
        assert "functions" not in payload

    def test_explicit_summary_forwards_native_summary(self):
        tool, connection = self._tool(_detail_handler)

        payload = json.loads(tool(class_name="AActor", detail="summary"))

        assert [params.get("detail") for params in connection.detail_requests()] == ["summary"]
        assert payload["name"] == "AActor"
        assert payload["parent"] == "UObject"
        assert "properties" not in payload and "functions" not in payload

    def test_properties_view_still_uses_native_full_extraction(self):
        tool, connection = self._tool(_detail_handler)

        payload = json.loads(tool(class_name="AMyActor", detail="properties"))

        # The retained properties view keeps its existing projection: native full (or the native
        # default) followed by the established Python allowlist.
        assert connection.detail_requests()[-1].get("detail") in (None, "full")
        assert "properties" in payload
        assert "components" in payload
        assert "functions" not in payload

    def test_full_view_recovers_declared_members(self):
        tool, connection = self._tool(_detail_handler)

        payload = json.loads(tool(class_name="AMyActor", detail="full"))

        assert connection.detail_requests()[-1].get("detail") in (None, "full")
        assert "properties" in payload
        assert "functions" in payload

    def test_invalid_detail_refuses_without_native_extraction(self):
        tool, connection = self._tool(_detail_handler)

        result = tool(class_name="AMyActor", detail="verbose")

        assert result.startswith("Error"), result
        assert connection.requests == []

    def test_projection_keeps_coverage_and_error_metadata(self):
        def handler(command, params):
            data = copy.deepcopy(_FULL_DETAIL)
            data["cdo_unavailable"] = True
            data["components"] = []
            data["_message"] = "component defaults unavailable"
            return data

        tool, _ = self._tool(handler)

        payload = json.loads(tool(class_name="AMyActor", detail="full"))

        assert payload["cdo_unavailable"] is True
        assert payload["_message"] == "component defaults unavailable"


class TestContextNativeSummaryAndChildBounds:
    """query_class_context fetches a parent summary and bounds children honestly."""

    @staticmethod
    def _tool(handler):
        from reflect.context import register_reflect_context_tools

        mcp = MockMCP()
        connection = StubReflectConnection(handler)
        register_reflect_context_tools(mcp, connection)
        return mcp.tools["query_class_context"], connection

    def test_parent_is_fetched_through_native_summary(self):
        tool, connection = self._tool(_context_handler)

        payload = json.loads(tool(class_name="AMyActor"))

        parent_request = next(
            params for params in connection.detail_requests() if params["class_name"] == "AActor"
        )
        self_request = next(
            params for params in connection.detail_requests() if params["class_name"] == "AMyActor"
        )
        assert parent_request.get("detail") == "summary"
        assert self_request.get("detail") in (None, "full")

        assert payload["self"]["name"] == "AMyActor"
        assert payload["self"]["properties"]
        assert payload["parent"]["name"] == "AActor"
        assert "properties" not in payload["parent"]
        assert "functions" not in payload["parent"]

    def test_bounded_children_declare_incompleteness(self):
        tool, _ = self._tool(_context_handler)

        payload = json.loads(tool(class_name="APawn"))

        assert payload["children"]
        assert payload["children_count"] == len(payload["children"])
        # A capped child read must never be presented as a complete child inventory.
        assert (
            payload.get("children_truncated") is True
            or payload.get("children_complete") is False
        ), payload

    def test_complete_children_read_is_not_marked_truncated(self):
        def handler(command, params):
            if command == "reflect.class_hierarchy":
                return {
                    "name": params["root"],
                    "type": "cpp",
                    "children": [{"name": "Child_0", "type": "cpp"}],
                    "total_classes": 2,
                    "cpp_count": 2,
                    "blueprint_count": 0,
                }
            return _FULL_DETAIL

        tool, _ = self._tool(handler)

        payload = json.loads(tool(class_name="AMyActor"))

        assert payload["children_count"] == 1
        assert payload.get("children_truncated") in (None, False)
        assert payload.get("children_complete") in (None, True)

    def test_truncated_below_emitted_cap_declares_incompleteness(self):
        # The native node budget can be spent on filtered/skipped intermediaries and deeper nodes,
        # so a bounded walk may emit far fewer children than the reader's requested cap. The reader
        # must trust the native traversal-exhaustion signal, not the emitted length.
        def handler(command, params):
            if command == "reflect.class_hierarchy":
                return {
                    "name": params["root"],
                    "type": "cpp",
                    # 3 children are far below the reader's 20-node cap, yet the traversal was
                    # bounded natively, so the page must not be presented as complete.
                    "children": [{"name": f"Child_{i}", "type": "cpp"} for i in range(3)],
                    "total_classes": 42,
                    "cpp_count": 42,
                    "blueprint_count": 0,
                    "truncated": True,
                }
            return _FULL_DETAIL

        tool, _ = self._tool(handler)

        payload = json.loads(tool(class_name="AMyActor"))

        assert payload["children_count"] == 3
        assert payload["children_complete"] is False


class TestReferencersIncludeConfig:
    """The standalone get_referencers wrapper forwards include_config only when provided."""

    @staticmethod
    def _tool():
        from reflect.referencers import register_reflect_referencer_tools

        mcp = MockMCP()
        connection = StubReflectConnection(lambda command, params: {"referencers": [], "total": 0})
        register_reflect_referencer_tools(mcp, connection)
        return mcp.tools["get_referencers"], connection

    def test_omitted_include_config_is_not_forwarded(self):
        tool, connection = self._tool()

        tool(asset_path="/Game/BP_Arm")

        (_, params), = connection.requests
        assert "include_config" not in params

    @pytest.mark.parametrize("value", [True, False])
    def test_provided_include_config_is_forwarded(self, value):
        tool, connection = self._tool()

        tool(asset_path="/Game/BP_Arm", include_config=value)

        (_, params), = connection.requests
        assert params["include_config"] is value
