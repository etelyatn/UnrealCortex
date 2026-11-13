"""Tests for gen_compose submit-and-wait composite."""

from __future__ import annotations

import asyncio
import json
from unittest.mock import MagicMock

import pytest

from cortex_mcp.tcp_client import UEConnection, UECommandError


def _connection(send_command):
    """A connection limited to the real UEConnection API (it has no async ``send``)."""
    conn = MagicMock(spec=UEConnection)
    conn.send_command.side_effect = send_command
    return conn


def _make_mock_connection():
    """Create a mock UEConnection that simulates job lifecycle."""
    call_count = 0

    def fake_send(command: str, params: dict | None = None):
        nonlocal call_count
        if command == "gen.start_mesh":
            return {
                "success": True,
                "data": {"job_id": "gen_abc12345", "status": "pending", "provider": "fal"},
            }
        elif command == "gen.job_status":
            call_count += 1
            if call_count < 3:
                return {
                    "success": True,
                    "data": {"job_id": "gen_abc12345", "status": "processing", "progress": 0.5},
                }
            else:
                return {
                    "success": True,
                    "data": {
                        "job_id": "gen_abc12345",
                        "status": "imported",
                        "progress": 1.0,
                        "asset_paths": ["/Game/Generated/Meshes/gen_abc12345"],
                    },
                }
        return {"success": False, "error": "Unknown command"}

    return _connection(fake_send)


def _make_failed_connection():
    """Mock connection where generation fails."""

    def fake_send(command: str, params: dict | None = None):
        if command == "gen.start_mesh":
            return {
                "success": True,
                "data": {"job_id": "gen_fail123", "status": "pending", "provider": "fal"},
            }
        elif command == "gen.job_status":
            return {
                "success": True,
                "data": {
                    "job_id": "gen_fail123",
                    "status": "failed",
                    "error": "Provider returned 500",
                },
            }
        return {"success": False, "error": "Unknown"}

    return _connection(fake_send)


def test_gen_compose_module_exists():
    """Verify the gen compose module can be imported."""
    from cortex_mcp.tools.composites.gen import register_gen_compose_tools
    assert callable(register_gen_compose_tools)


def test_gen_compose_registration():
    """Verify tools are registered on a mock MCP server."""
    from cortex_mcp.tools.composites.gen import register_gen_compose_tools

    registered = {}

    class FakeMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                registered[name or fn.__name__] = fn
                return fn
            return decorator

    register_gen_compose_tools(FakeMCP(), MagicMock())
    assert "gen_compose" in registered


def test_submit_and_wait_success():
    """Test successful submit-poll-complete lifecycle."""
    from cortex_mcp.tools.composites.gen import _submit_and_wait

    conn = _make_mock_connection()
    result = asyncio.run(_submit_and_wait(
        conn, "gen.start_mesh", {"prompt": "a barrel"},
        poll_interval=0.01, timeout=5.0,
    ))
    data = json.loads(result)
    assert data["success"] is True
    assert data["data"]["status"] == "imported"
    assert data["data"]["job_id"] == "gen_abc12345"
    assert "/Game/Generated/Meshes/gen_abc12345" in data["data"]["asset_paths"]


def test_submit_and_wait_failure():
    """Test that failed jobs return terminal state immediately."""
    from cortex_mcp.tools.composites.gen import _submit_and_wait

    conn = _make_failed_connection()
    result = asyncio.run(_submit_and_wait(
        conn, "gen.start_mesh", {"prompt": "test"},
        poll_interval=0.01, timeout=5.0,
    ))
    data = json.loads(result)
    assert data["success"] is True  # poll succeeded, job status is "failed"
    assert data["data"]["status"] == "failed"
    assert data["data"]["job_id"] == "gen_fail123"


def test_submit_and_wait_timeout():
    """Test timeout when job never reaches terminal state."""
    from cortex_mcp.tools.composites.gen import _submit_and_wait


    def fake_send(command: str, params=None):
        if command == "gen.start_mesh":
            return {
                "success": True,
                "data": {"job_id": "gen_timeout", "status": "pending", "provider": "fal"},
            }
        elif command == "gen.job_status":
            return {
                "success": True,
                "data": {"job_id": "gen_timeout", "status": "processing", "progress": 0.1},
            }
        return {"success": False, "error": "Unknown"}

    conn = _connection(fake_send)

    result = asyncio.run(_submit_and_wait(
        conn, "gen.start_mesh", {"prompt": "slow"},
        poll_interval=0.01, timeout=0.05,
    ))
    data = json.loads(result)
    assert data["success"] is False
    assert "Timeout" in data["error"]
    assert data["data"]["status"] == "timeout"


def test_submit_and_wait_submit_fails():
    """Test that a failed submit returns the error immediately."""
    from cortex_mcp.tools.composites.gen import _submit_and_wait


    def fake_send(command: str, params=None):
        raise UECommandError(command, "PROVIDER_NOT_FOUND", "No provider configured")

    conn = _connection(fake_send)

    result = asyncio.run(_submit_and_wait(
        conn, "gen.start_mesh", {"prompt": "test"},
        poll_interval=0.01, timeout=5.0,
    ))
    data = json.loads(result)
    assert data["success"] is False
    assert "PROVIDER_NOT_FOUND" in data["error"]


def test_gen_compose_invalid_type():
    """Test gen_compose rejects unknown generation types."""
    from cortex_mcp.tools.composites.gen import register_gen_compose_tools

    registered = {}

    class FakeMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                registered[name or fn.__name__] = fn
                return fn
            return decorator

    register_gen_compose_tools(FakeMCP(), MagicMock())
    gen_compose = registered["gen_compose"]

    result = asyncio.run(gen_compose(type="unknown"))
    data = json.loads(result)
    assert data["success"] is False
    assert "Unknown gen type" in data["error"]


def test_submit_and_wait_reports_connection_failure():
    """A lost editor connection is returned as a result, not raised out of the tool."""
    from cortex_mcp.tools.composites.gen import _submit_and_wait

    def fake_send(command: str, params=None):
        raise ConnectionError("No running Unreal Editor found.")

    result = asyncio.run(_submit_and_wait(
        _connection(fake_send), "gen.start_mesh", {"prompt": "test"},
        poll_interval=0.01, timeout=5.0,
    ))
    data = json.loads(result)
    assert data["success"] is False
    assert "No running Unreal Editor" in data["error"]


def test_gen_compose_uses_the_real_connection_api():
    """gen_compose must only call methods UEConnection actually has."""
    from cortex_mcp.tools.composites.gen import register_gen_compose_tools

    registered = {}

    class FakeMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                registered[name or fn.__name__] = fn
                return fn
            return decorator

    def fake_send(command: str, params=None):
        if command == "gen.start_image":
            return {"success": True, "data": {"job_id": "gen_img", "status": "imported"}}
        return {"success": True, "data": {"job_id": "gen_img", "status": "imported"}}

    conn = _connection(fake_send)
    register_gen_compose_tools(FakeMCP(), conn)

    import cortex_mcp.tools.composites.gen as gen_module
    original_sleep = gen_module.asyncio.sleep

    async def no_sleep(_seconds):
        await original_sleep(0)

    gen_module.asyncio.sleep = no_sleep
    try:
        result = asyncio.run(registered["gen_compose"](type="image", prompt="a crate"))
    finally:
        gen_module.asyncio.sleep = original_sleep

    data = json.loads(result)
    assert data["success"] is True
    assert data["data"]["status"] == "imported"
    assert conn.send_command.call_args_list[0].args == ("gen.start_image", {"prompt": "a crate"})

