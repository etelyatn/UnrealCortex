"""Unit tests for QA MCP tools."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

# Add tools/ to path for imports (matching existing tests)
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from qa.actions import register_qa_action_tools
from qa.assertions import register_qa_assertion_tools
from qa.composites import register_qa_composite_tools
from qa.detector import detect_structural_issues
from qa.reporter import write_report_bundle
from qa.setup import register_qa_setup_tools
from qa.world import register_qa_world_tools


class MockMCP:
    """Captures tools registered via @mcp.tool()."""

    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def test_world_tools_registration_and_wiring():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"actor_count": 1}}

    register_qa_world_tools(mcp, connection)

    assert "observe_game_state" in mcp.tools
    assert "get_actor_details" in mcp.tools
    assert "get_player_details" in mcp.tools

    result = mcp.tools["observe_game_state"](include_line_of_sight=True)
    parsed = json.loads(result)
    assert parsed["actor_count"] == 1
    connection.send_command.assert_called_with(
        "qa.observe_state", {"include_line_of_sight": True}
    )


def test_action_tools_send_deferred_timeouts():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"arrived": True}}

    register_qa_action_tools(mcp, connection)
    result = mcp.tools["move_player_to"](
        target=[100.0, 0.0, 0.0], timeout=7.0, acceptance_radius=50.0
    )

    parsed = json.loads(result)
    assert parsed["arrived"] is True
    connection.send_command.assert_called_with(
        "qa.move_to",
        {"target": [100.0, 0.0, 0.0], "timeout": 7.0, "acceptance_radius": 50.0},
        timeout=12.0,
    )


def test_wait_for_condition_timeout_forwarding():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"condition_met": True}}

    register_qa_action_tools(mcp, connection)
    result = mcp.tools["wait_for_condition"](
        type="actor_visible",
        timeout=9.0,
        actor="Door_01",
    )

    parsed = json.loads(result)
    assert parsed["condition_met"] is True
    connection.send_command.assert_called_with(
        "qa.wait_for",
        {"type": "actor_visible", "timeout": 9.0, "actor": "Door_01"},
        timeout=14.0,
    )


def test_setup_tools_wiring():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"ok": True}}

    register_qa_setup_tools(mcp, connection)
    mcp.tools["set_random_seed"](123)
    connection.send_command.assert_called_with("qa.set_random_seed", {"seed": 123})


def test_assertion_tool_captures_screenshot_on_failure():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.side_effect = [
        {"data": {"passed": False, "message": "failed"}},
        {"data": {"path": "Saved/Screenshots/qa_assertion_failed.png"}},
    ]

    register_qa_assertion_tools(mcp, connection)
    result = mcp.tools["assert_game_state"](type="actor_visible", actor="Door_01")
    parsed = json.loads(result)
    assert parsed["passed"] is False

    assert connection.send_command.call_count == 2
    assert connection.send_command.call_args_list[1].args[0] == "editor.capture_screenshot"


def test_composite_test_step_aggregates_summary():
    mcp = MockMCP()
    connection = MagicMock()

    def send_command(command, params=None, timeout=None):
        if command == "qa.interact":
            return {"success": True, "data": {"success": True}}
        if command == "qa.wait_for":
            return {"success": True, "data": {"timed_out": False}}
        if command == "qa.observe_state":
            return {"success": True, "data": {"actors": [], "player": {"location": [0, 0, 0]}}}
        if command == "editor.get_recent_logs":
            return {"success": True, "data": {"logs": []}}
        if command == "qa.assert_state":
            return {"success": True, "data": {"passed": True}}
        if command == "editor.capture_screenshot":
            return {"success": True, "data": {"path": "Saved/Screenshots/qa_step.png"}}
        raise AssertionError(f"Unexpected command: {command}")

    connection.send_command.side_effect = send_command
    register_qa_composite_tools(mcp, connection)

    result = mcp.tools["test_step"](
        action={"command": "qa.interact", "params": {"key": "E"}},
        wait={"command": "qa.wait_for", "params": {"type": "delay", "timeout": 1.0}},
        assertion={"command": "qa.assert_state", "params": {"type": "delay", "expected": True}},
    )
    parsed = json.loads(result)
    assert parsed["action_success"] is True
    assert parsed["assert_success"] is True
    assert parsed["finding_count"] == 0


def test_run_scenario_inline_uses_log_cursor_and_writes_report():
    mcp = MockMCP()
    connection = MagicMock()

    call_state = {"cursor": 0}

    def send_command(command, params=None, timeout=None):
        if command == "qa.look_at":
            return {"success": True, "data": {"success": True}}
        if command == "qa.wait_for":
            return {"success": True, "data": {"timed_out": False}}
        if command == "qa.observe_state":
            return {"success": True, "data": {"actors": [], "player": {"location": [0, 0, 0]}}}
        if command == "editor.get_recent_logs":
            expected = call_state["cursor"]
            assert params["cursor"] == expected
            call_state["cursor"] = expected + 10
            return {"success": True, "data": {"logs": [], "next_cursor": call_state["cursor"]}}
        raise AssertionError(f"Unexpected command: {command}")

    connection.send_command.side_effect = send_command
    register_qa_composite_tools(mcp, connection)

    scenario = [
        {"command": "qa.look_at", "params": {"target": "Door_01"}},
        {"command": "qa.wait_for", "params": {"type": "delay", "timeout": 1.0}},
    ]
    with patch(
        "qa.composites.write_report_bundle",
        return_value={
            "report_dir": "QA/reports/mock",
            "summary_path": "QA/reports/mock/summary.md",
            "findings_path": "QA/reports/mock/findings.json",
            "finding_count": 0,
        },
    ):
        result = mcp.tools["run_scenario_inline"](
            scenario_name="qa-inline",
            steps=scenario,
            verbose=False,
        )
    parsed = json.loads(result)
    assert parsed["scenario"] == "qa-inline"
    assert len(parsed["steps"]) == 2
    assert parsed["report"]["finding_count"] == 0


def test_detector_finds_multiple_issue_types():
    findings = detect_structural_issues(
        observed_state={
            "player": {"location": [0.0, 0.0, -20000.0]},
            "actors": [{"name": "BadActor", "location": [0.2, -0.1, 0.5]}],
            "world": {"fps": 10.0},
        },
        wait_result={"timed_out": True},
        recent_logs={"logs": [{"level": "Error", "message": "Error: failure"}]},
    )

    categories = {f["category"] for f in findings}
    assert "physics" in categories
    assert "placement" in categories
    assert "timeout" in categories
    assert "performance" in categories
    assert "logs" in categories


def test_reporter_writes_summary_and_findings(tmp_path):
    bundle = write_report_bundle(
        scenario_name="Door Smoke",
        summary_lines=["# Summary", "- Pass: 1"],
        findings=[{"severity": "MINOR", "summary": "ok"}],
        root=tmp_path,
    )

    assert Path(bundle["summary_path"]).exists()
    assert Path(bundle["findings_path"]).exists()

    summary = Path(bundle["summary_path"]).read_text(encoding="utf-8")
    findings = json.loads(Path(bundle["findings_path"]).read_text(encoding="utf-8"))
    assert "# Summary" in summary
    assert findings[0]["severity"] == "MINOR"
