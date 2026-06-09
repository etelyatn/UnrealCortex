import json
from unittest.mock import MagicMock

from cortex_mcp.tcp_client import UECommandError
from tools.data.import_queues import register_import_queue_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self, name=None, description=None, **_kwargs):
        def decorator(func):
            self.tools[name or func.__name__] = func
            return func

        return decorator


def test_apply_import_ops_json_defaults_to_dry_run():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {
        "success": True,
        "data": {
            "status": "dry_run_ok",
            "success": True,
            "partial": False,
            "dry_run": True,
            "applied": False,
            "operation_count": 0,
            "report_path": "Saved/CortexImports/report.json",
        },
    }

    register_import_queue_tools(mcp, connection)
    result = json.loads(mcp.tools["apply_import_ops_json"](
        "Saved/CortexImports/ops.json",
        "Saved/CortexImports/report.json",
    ))

    assert result["dry_run"] is True
    connection.send_command.assert_called_once_with(
        "data.apply_import_ops_json",
        {
            "ops_path": "Saved/CortexImports/ops.json",
            "report_path": "Saved/CortexImports/report.json",
            "dry_run": True,
            "apply": False,
            "stop_on_error": True,
            "query_back": True,
            "allow_partial": False,
        },
    )


def test_apply_import_ops_json_returns_structured_failure_details():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.side_effect = UECommandError(
        "data.apply_import_ops_json",
        "REPORT_WRITE_FAILED",
        "Failed to write report",
        {
            "status": "report_write_failed",
            "attempted_count": 1,
            "applied_count": 1,
            "failed_count": 0,
            "last_operation_index": 0,
            "first_error": "Failed to write report",
        },
    )

    register_import_queue_tools(mcp, connection)
    result = json.loads(mcp.tools["apply_import_ops_json"](
        "Saved/CortexImports/ops.json",
        "Saved/CortexImports/report.json",
        dry_run=False,
        apply=True,
    ))

    assert result["success"] is False
    assert result["_error"] == "REPORT_WRITE_FAILED"
    assert result["_command"] == "data.apply_import_ops_json"
    assert result["status"] == "report_write_failed"
    assert result["applied_count"] == 1


def test_apply_import_ops_json_preserves_reserved_failure_fields():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.side_effect = UECommandError(
        "data.apply_import_ops_json",
        "REPORT_WRITE_FAILED",
        "Failed to write report",
        {
            "success": True,
            "_error": "OVERRIDE",
            "_message": "override",
            "_command": "override.command",
            "status": "report_write_failed",
        },
    )

    register_import_queue_tools(mcp, connection)
    result = json.loads(mcp.tools["apply_import_ops_json"](
        "Saved/CortexImports/ops.json",
        "Saved/CortexImports/report.json",
        dry_run=False,
        apply=True,
    ))

    assert result["success"] is False
    assert result["_error"] == "REPORT_WRITE_FAILED"
    assert result["_message"] == "Failed to write report"
    assert result["_command"] == "data.apply_import_ops_json"
    assert result["status"] == "report_write_failed"
