"""Tests for gen_compose submit-and-wait composite."""

from __future__ import annotations

import asyncio
import json
from unittest.mock import AsyncMock, MagicMock

import pytest


@pytest.fixture
def mock_connection():
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


@pytest.fixture
def failed_connection():
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
