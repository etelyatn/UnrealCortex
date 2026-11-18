"""Unit tests for reflect tool registration and response formatting.

The six thin wrappers over reflect_cmd-listed commands were removed (issue #225); these
tests cover the surviving standalones: cache, context and impact.
"""

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

    def test_impact_registration(self):
        from reflect.impact import register_reflect_impact_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_impact_tools(mcp, connection)
        mcp.tool.assert_called()

    def test_init_registers_all(self):
        from reflect import register_reflect_tools

        mcp = MagicMock()
        connection = MagicMock()
        register_reflect_tools(mcp, connection)
        assert mcp.tool.call_count >= 3


class TestDetailProjection:
    """The context tool's detail levels reuse the established projection allowlists."""

    def test_summary_level(self):
        from reflect.context import _prune_detail

        pruned = _prune_detail(_FULL_DETAIL, "summary")

        assert pruned["name"] == "AMyActor"
        assert pruned["parent"] == "AActor"
        assert "properties" not in pruned
        assert "functions" not in pruned
        assert "components" not in pruned

    def test_properties_level(self):
        from reflect.context import _prune_detail

        pruned = _prune_detail(_FULL_DETAIL, "properties")

        assert pruned["name"] == "AMyActor"
        assert "properties" in pruned
        assert "components" in pruned
        assert "functions" not in pruned

    def test_full_level(self):
        from reflect.context import _prune_detail

        pruned = _prune_detail(_FULL_DETAIL, "full")

        assert "properties" in pruned
        assert "functions" in pruned
        assert "components" in pruned

    def test_projection_keeps_coverage_and_error_metadata(self):
        from reflect.context import _prune_detail

        data = copy.deepcopy(_FULL_DETAIL)
        data["cdo_unavailable"] = True
        data["_message"] = "component defaults unavailable"

        pruned = _prune_detail(data, "full")

        assert pruned["cdo_unavailable"] is True
        assert pruned["_message"] == "component defaults unavailable"


class TestContextNativeSummaryAndChildBounds:
    """query_class_context asks the native side for the declared view before extraction."""

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

        # AActor is the parent here: its class_detail read must be the reduced native summary.
        parent_requests = [
            params for params in connection.detail_requests()
            if params.get("class_name") == "AActor"
        ]
        assert parent_requests and all(
            params.get("detail") == "summary" for params in parent_requests
        )
        assert payload["parent"]["name"] == "AActor"
        assert "properties" not in payload["parent"]

    def test_self_defaults_to_properties_view_without_native_summary(self):
        tool, connection = self._tool(_context_handler)

        payload = json.loads(tool(class_name="AMyActor"))

        # The self view defaults to 'properties': native full extraction plus the allowlist.
        self_requests = [
            params for params in connection.detail_requests()
            if params.get("class_name") == "AMyActor"
        ]
        assert self_requests and self_requests[0].get("detail") in (None, "full")
        assert "properties" in payload["self"]
        assert "functions" not in payload["self"]

    def test_self_summary_targets_native_summary(self):
        tool, connection = self._tool(_context_handler)

        payload = json.loads(tool(class_name="AMyActor", detail="summary"))

        self_requests = [
            params for params in connection.detail_requests()
            if params.get("class_name") == "AMyActor"
        ]
        assert self_requests and self_requests[0].get("detail") == "summary"
        assert "properties" not in payload["self"]

    def test_self_full_view_recovers_declared_members(self):
        tool, connection = self._tool(_context_handler)

        payload = json.loads(tool(class_name="AMyActor", detail="full"))

        self_requests = [
            params for params in connection.detail_requests()
            if params.get("class_name") == "AMyActor"
        ]
        assert self_requests and self_requests[0].get("detail") in (None, "full")
        assert "functions" in payload["self"]

    def test_invalid_detail_refuses_without_native_extraction(self):
        tool, connection = self._tool(_context_handler)

        result = tool(class_name="AMyActor", detail="verbose")

        payload = json.loads(result)
        assert payload["success"] is False
        assert payload["_error"] == "INVALID_FIELD"
        assert "expected one of" in payload["_message"]
        assert connection.requests == []

    def test_bounded_children_declare_incompleteness(self):
        tool, _ = self._tool(_context_handler)

        payload = json.loads(tool(class_name="AMyActor"))

        assert payload["children_complete"] is False
        assert payload["children_count"] == _CHILDREN_MAX_RESULTS - 1

    def test_complete_children_read_is_not_marked_truncated(self):
        def handler(command, params):
            if command == "reflect.class_detail":
                return _FULL_DETAIL if params.get("class_name") != "AActor" else _SUMMARY_DETAIL
            if command == "reflect.class_hierarchy":
                return {
                    "name": params.get("root", "Unknown"),
                    "type": "cpp",
                    "children": [{"name": "Child_A", "type": "cpp"}],
                    "total_classes": 2,
                    "cpp_count": 2,
                    "blueprint_count": 0,
                    "truncated": False,
                }
            raise AssertionError(f"unexpected native command: {command}")

        tool, _ = self._tool(handler)

        payload = json.loads(tool(class_name="AMyActor"))

        assert payload["children_complete"] is True
        assert payload["children"] == [{"name": "Child_A", "type": "cpp"}]

    def test_truncated_below_emitted_cap_declares_incompleteness(self):
        def handler(command, params):
            if command == "reflect.class_detail":
                return _FULL_DETAIL if params.get("class_name") != "AActor" else _SUMMARY_DETAIL
            if command == "reflect.class_hierarchy":
                # Fewer children than the cap, but the native walk itself was cut: the
                # emitted count must not be mistaken for an exhausted traversal.
                return {
                    "name": params.get("root", "Unknown"),
                    "type": "cpp",
                    "children": [{"name": "Child_A", "type": "cpp"}],
                    "total_classes": 9,
                    "cpp_count": 9,
                    "blueprint_count": 0,
                    "truncated": True,
                }
            raise AssertionError(f"unexpected native command: {command}")

        tool, _ = self._tool(handler)

        payload = json.loads(tool(class_name="AMyActor"))

        assert payload["children_complete"] is False
        assert payload["children_count"] == 1


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

_CHILDREN_MAX_RESULTS = 20


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


def _context_handler(command, params):
    if command == "reflect.class_detail":
        # The parent is only reachable through its native summary; the requested class keeps the
        # full extraction that the retained properties default needs.
        if params.get("class_name") == "AActor":
            return _SUMMARY_DETAIL
        return _FULL_DETAIL
    if command == "reflect.class_hierarchy":
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
    raise AssertionError(f"unexpected native command: {command}")
