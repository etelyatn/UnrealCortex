"""Issue #227 point 6: qa_test_step documents and enforces its {command, params} step shape."""

from __future__ import annotations

import asyncio
import json

import pytest
from mcp.server.fastmcp import FastMCP

from cortex_mcp.tools.standalone.qa import register_qa_standalone_tools


class _RecordingConnection:
    def __init__(self):
        self.sent: list[str] = []

    def send_command(self, command, params=None, timeout=None):
        self.sent.append(command)
        return {"success": True, "data": {}}

    send_command_once = send_command


def _call(arguments: dict):
    async def scenario():
        mcp = FastMCP("probe")
        connection = _RecordingConnection()
        register_qa_standalone_tools(mcp, connection)
        tool = mcp._tool_manager.get_tool("qa_test_step")
        result = await mcp.call_tool("qa_test_step", arguments)
        return connection, tool, result[0][0].text

    return asyncio.run(scenario())


def test_description_documents_the_step_shape():
    _, tool, _ = _call({"action": {"command": "qa.observe_state"}})
    assert '{"command": "qa.<command>", "params": {...}}' in tool.description


@pytest.mark.parametrize(
    "arguments, field",
    [
        ({"action": {"params": {}}}, "action"),
        ({"action": {"command": ""}}, "action"),
        ({"action": {"command": "qa.interact", "params": []}}, "action"),
        ({"action": {"command": "qa.interact"}, "wait": {"params": {"timeout": 3}}}, "wait"),
        ({"action": {"command": "qa.interact"}, "assertion": {"command": 7}}, "assertion"),
    ],
)
def test_malformed_step_is_refused_before_anything_is_sent(arguments, field):
    connection, _, text = _call(arguments)
    assert connection.sent == []
    payload = json.loads(text)
    assert payload["_error"] == "INVALID_FIELD"
    assert field in payload["_message"]
    assert '{"command"' in payload["_message"]
