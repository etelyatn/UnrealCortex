"""Shared fixtures for MCP E2E and scenario tests.

All fixtures require a running Unreal Editor with UnrealCortex plugin.
The editor writes Saved/CortexPort.txt which the TCP client auto-discovers.
"""

import json
import os
import sys
import uuid

import pytest

# Ensure cortex_mcp is importable
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from cortex_mcp.tcp_client import UEConnection


# ---------------------------------------------------------------------------
# Layer 1: TCP E2E fixtures
# ---------------------------------------------------------------------------


def _create_temp_blueprint(tcp_connection, prefix: str, bp_type: str) -> str:
    """Create a uniquely named temp Blueprint to avoid cross-run collisions."""
    resp = tcp_connection.send_command("bp.create", {
        "name": f"{prefix}_{uuid.uuid4().hex[:8]}",
        "path": "/Game/Temp/CortexMCPTest",
        "type": bp_type,
    })
    return resp["data"]["asset_path"]


@pytest.fixture(scope="session")
def tcp_connection():
    """Session-wide TCP connection to Unreal Editor.

    Auto-discovers port from Saved/CortexPort.txt.
    Auto-connects on first send_command() call.
    """
    conn = UEConnection()
    yield conn
    conn.disconnect()


@pytest.fixture(scope="class")
def blueprint_for_test(tcp_connection):
    """Create a temporary Actor BP for Blueprint/Graph CRUD tests.

    Created once per test class, deleted on teardown.
    """
    asset_path = _create_temp_blueprint(tcp_connection, "BP_E2E_Fixture", "Actor")
    yield asset_path
    try:
        tcp_connection.send_command("bp.delete", {
            "asset_path": asset_path, "force": True,
        })
    except (RuntimeError, ConnectionError):
        pass


@pytest.fixture(scope="class")
def widget_bp_for_test(tcp_connection):
    """Create a temporary Widget BP for UMG CRUD tests.

    Created once per test class, deleted on teardown.
    """
    asset_path = _create_temp_blueprint(tcp_connection, "WBP_E2E_Fixture", "Widget")
    yield asset_path
    try:
        tcp_connection.send_command("bp.delete", {
            "asset_path": asset_path, "force": True,
        })
    except (RuntimeError, ConnectionError):
        pass


@pytest.fixture()
def cleanup_assets(tcp_connection):
    """Collect created asset paths during a test, delete all on teardown."""
    created = []
    yield created
    for path in reversed(created):
        try:
            tcp_connection.send_command("bp.delete", {
                "asset_path": path, "force": True,
            })
        except (RuntimeError, ConnectionError):
            pass


# ---------------------------------------------------------------------------
# Layer 2: MCP Scenario fixtures
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def anyio_backend():
    return "asyncio"


@pytest.fixture(scope="module")
async def mcp_client():
    """MCP test client connected to the cortex_mcp server.

    The server's TCP connection auto-discovers the running Unreal Editor.
    All registered MCP tools are available through client.call_tool().
    """
    try:
        from mcp import Client
    except ImportError:
        pytest.skip("mcp.Client is unavailable in this environment/version")
    from cortex_mcp.server import mcp as mcp_app

    async with Client(mcp_app, raise_exceptions=True) as c:
        yield c


async def call_tool_json(client, tool_name: str, arguments: dict) -> dict:
    """Helper: call an MCP tool and parse the JSON response."""
    result = await client.call_tool(tool_name, arguments)
    text = result.content[0].text
    return json.loads(text)
