"""The standalone hierarchy tool must preserve the native traversal-completeness signal.

`reflect.class_hierarchy` emits an authoritative `truncated` flag produced by the walk itself (true
when the depth/max_results walk stopped early). The standalone tool projects the native tree down to
a flat class list, so an emitted child count is not a substitute: without the flag a caller cannot
tell an exhausted traversal from a budget-cut one.
"""


def _tree(**overrides):
    tree = {
        "name": "AActor",
        "type": "cpp",
        "total_classes": 3,
        "cpp_count": 2,
        "blueprint_count": 1,
        "truncated": False,
        "children": [
            {"name": "APawn", "type": "cpp", "children": []},
            {
                "name": "BP_Thing_C",
                "type": "blueprint",
                "asset_path": "/Game/BP_Thing",
                "children": [],
            },
        ],
    }
    tree.update(overrides)
    return tree


def test_flattened_hierarchy_reports_an_exhausted_traversal():
    from reflect.hierarchy import _flatten_hierarchy

    result = _flatten_hierarchy(_tree())

    assert result["truncated"] is False
    assert result["total_classes"] == 3
    assert [entry["name"] for entry in result["classes"]] == ["AActor", "APawn", "BP_Thing_C"]


def test_flattened_hierarchy_reports_a_truncated_traversal_distinguishably():
    from reflect.hierarchy import _flatten_hierarchy

    complete = _flatten_hierarchy(_tree())
    cut = _flatten_hierarchy(_tree(truncated=True))

    assert cut["truncated"] is True
    assert complete != cut, "a budget-cut traversal must not look identical to a complete one"


def test_registered_standalone_tool_forwards_the_completeness_signal():
    """The tool the server actually registers must expose the signal, not just the projection."""
    import json
    from unittest.mock import MagicMock

    from reflect.hierarchy import register_reflect_hierarchy_tools

    class _MCP:
        def __init__(self):
            self.tools = {}

        def tool(self, name=None, **_kwargs):
            def decorator(fn):
                self.tools[name or fn.__name__] = fn
                return fn

            return decorator

    connection = MagicMock()
    connection.send_command_cached.return_value = {"success": True, "data": _tree(truncated=True)}
    mcp = _MCP()
    register_reflect_hierarchy_tools(mcp, connection)

    payload = json.loads(mcp.tools["query_class_hierarchy"]("AActor", 3, True, False, 100))

    assert payload["truncated"] is True
    assert payload["total_classes"] == 3
