"""Unit tests for PIE primitive tools."""

import os
import sys
from unittest.mock import MagicMock, call

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "editor"))

from cortex_mcp.tcp_client import UEConnection
from pie import register_editor_pie_tools


class MockMCP:
    """Captures tools registered via @mcp.tool()."""

    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


class TestStartPieFPSThrottle:
    """start_pie should inject FPS throttle console commands after PIE starts."""

    def test_start_pie_sends_fps_throttle_commands(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.return_value = {
            "success": True,
            "data": {"state": "Playing", "mode": "selected_viewport"},
        }

        mcp = MockMCP()
        register_editor_pie_tools(mcp, conn)

        mcp.tools["start_pie"]()

        expected_calls = [
            call("editor.start_pie", {"mode": "selected_viewport"}, timeout=60.0),
            call("editor.execute_console_command", {"command": "t.MaxFPS 0"}),
            call("editor.execute_console_command", {"command": "t.UnfocusedFrameRateLimit 0"}),
        ]
        conn.send_command.assert_has_calls(expected_calls)

    def test_start_pie_throttle_failure_is_non_fatal(self):
        conn = MagicMock(spec=UEConnection)
        call_count = 0

        def side_effect(*args, **kwargs):
            nonlocal call_count
            call_count += 1
            if call_count == 1:
                return {
                    "success": True,
                    "data": {"state": "Playing", "mode": "selected_viewport"},
                }
            raise RuntimeError("Console command failed")

        conn.send_command.side_effect = side_effect

        mcp = MockMCP()
        register_editor_pie_tools(mcp, conn)

        result = mcp.tools["start_pie"]()
        assert "Error" not in result


class TestRestartPieFPSThrottle:
    """restart_pie should also inject FPS throttle commands."""

    def test_restart_pie_sends_fps_throttle_commands(self):
        conn = MagicMock(spec=UEConnection)
        conn.send_command.return_value = {
            "success": True,
            "data": {"state": "Playing", "mode": "selected_viewport"},
        }

        mcp = MockMCP()
        register_editor_pie_tools(mcp, conn)

        mcp.tools["restart_pie"]()

        expected_calls = [
            call("editor.restart_pie", {"mode": "selected_viewport"}, timeout=90.0),
            call("editor.execute_console_command", {"command": "t.MaxFPS 0"}),
            call("editor.execute_console_command", {"command": "t.UnfocusedFrameRateLimit 0"}),
        ]
        conn.send_command.assert_has_calls(expected_calls)

    def test_restart_pie_throttle_failure_is_non_fatal(self):
        conn = MagicMock(spec=UEConnection)
        call_count = 0

        def side_effect(*args, **kwargs):
            nonlocal call_count
            call_count += 1
            if call_count == 1:
                return {
                    "success": True,
                    "data": {"state": "Playing", "mode": "selected_viewport"},
                }
            raise RuntimeError("Console command failed")

        conn.send_command.side_effect = side_effect

        mcp = MockMCP()
        register_editor_pie_tools(mcp, conn)

        result = mcp.tools["restart_pie"]()
        assert "Error" not in result
