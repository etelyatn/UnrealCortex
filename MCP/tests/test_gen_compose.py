"""Tests for gen_compose submit-and-wait composite."""

from __future__ import annotations

import asyncio
import json
from unittest.mock import AsyncMock, MagicMock

import pytest


def _make_mock_connection():
    """Create a mock UEConnection that simulates job lifecycle."""
    conn = AsyncMock()
    call_count = 0

    async def fake_send(command: str, params: dict | None = None):
        nonlocal call_count
        if command == "gen.start_mesh":
            return json.dumps({
                "success": True,
                "data": {"job_id": "gen_abc12345", "status": "pending", "provider": "fal"},
            })
        elif command == "gen.job_status":
            call_count += 1
            if call_count < 3:
                return json.dumps({
                    "success": True,
                    "data": {"job_id": "gen_abc12345", "status": "processing", "progress": 0.5},
                })
            else:
                return json.dumps({
                    "success": True,
                    "data": {
                        "job_id": "gen_abc12345",
                        "status": "imported",
                        "progress": 1.0,
                        "asset_paths": ["/Game/Generated/Meshes/gen_abc12345"],
                    },
                })
        return json.dumps({"success": False, "error": "Unknown command"})

    conn.send = fake_send
    return conn


def _make_failed_connection():
    """Mock connection where generation fails."""
    conn = AsyncMock()

    async def fake_send(command: str, params: dict | None = None):
        if command == "gen.start_mesh":
            return json.dumps({
                "success": True,
                "data": {"job_id": "gen_fail123", "status": "pending", "provider": "fal"},
            })
        elif command == "gen.job_status":
            return json.dumps({
                "success": True,
                "data": {
                    "job_id": "gen_fail123",
                    "status": "failed",
                    "error": "Provider returned 500",
                },
            })
        return json.dumps({"success": False, "error": "Unknown"})

    conn.send = fake_send
    return conn


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

    conn = AsyncMock()

    async def fake_send(command: str, params=None):
        if command == "gen.start_mesh":
            return json.dumps({
                "success": True,
                "data": {"job_id": "gen_timeout", "status": "pending", "provider": "fal"},
            })
        elif command == "gen.job_status":
            return json.dumps({
                "success": True,
                "data": {"job_id": "gen_timeout", "status": "processing", "progress": 0.1},
            })
        return json.dumps({"success": False, "error": "Unknown"})

    conn.send = fake_send

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

    conn = AsyncMock()

    async def fake_send(command: str, params=None):
        return json.dumps({
            "success": False,
            "error": "PROVIDER_NOT_FOUND",
        })

    conn.send = fake_send

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
