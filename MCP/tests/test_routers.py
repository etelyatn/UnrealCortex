"""Unit tests for consolidated domain routers."""

import json
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from cortex_mcp.tools.routers import DOMAINS, make_router, register_router_tools
from cortex_mcp.tcp_client import EditorConnection


class MockMCP:
    """Captures tools registered via @mcp.tool()."""

    def __init__(self):
        self.tools = {}

    def tool(self, name=None, description=None, **_kwargs):
        def decorator(fn):
            self.tools[name or fn.__name__] = {"fn": fn, "description": description}
            return fn

        return decorator


def _editor(port: int, pid: int, started_at: str) -> EditorConnection:
    return EditorConnection(
        port=port,
        pid=pid,
        started_at=started_at,
        port_file=Path(f"CortexPort-{pid}.txt"),
    )


def test_make_router_dispatches_domain_command():
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": {"ok": True}}

    router = make_router("data", connection, "data docs")
    result = json.loads(router("query_datatable", {"table_path": "/Game/Data/DT_Test"}))

    assert result["ok"] is True
    connection.send_command.assert_called_once_with(
        "data.query_datatable",
        {"table_path": "/Game/Data/DT_Test"},
    )


def test_core_router_handles_switch_editor_locally():
    connection = MagicMock()
    connection.port = 8742
    connection._pid = 1000

    router = make_router("core", connection, "core docs")
    editors = [
        _editor(8742, 1000, "2026-01-01T00:00:00Z"),
        _editor(8743, 2000, "2026-01-01T01:00:00Z"),
    ]
    with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=editors), patch(
        "cortex_mcp.tools.routers._is_editor_alive", return_value=True
    ):
        payload = json.loads(router("switch_editor", {"pid": 2000}))

    connection.disconnect.assert_called_once()
    connection.send_command.assert_not_called()
    assert payload["pid"] == 2000
    assert connection.port == 8743


def test_core_router_handles_schema_status_locally(tmp_path):
    schema_dir = tmp_path / ".cortex" / "schema"
    schema_dir.mkdir(parents=True)
    (schema_dir / "_catalog.md").write_text(
        "---\ngenerated: 2026-03-14T12:00:00Z\nschema_version: 1\n---\n",
        encoding="utf-8",
    )

    router = make_router("core", MagicMock(), "core docs")
    with patch("cortex_mcp.tools.routers.get_schema_dir", return_value=schema_dir), patch(
        "cortex_mcp.tools.routers.read_meta_from_file",
        return_value={"generated": "2026-03-14T12:00:00Z", "schema_version": "1"},
    ):
        payload = json.loads(router("schema_status"))

    assert payload["exists"] is True
    assert payload["schema_dir"] == str(schema_dir)


def test_core_router_enriches_get_status_with_editor_discovery():
    connection = MagicMock()
    connection.port = 8743
    connection._pid = 2000
    connection.send_command.return_value = {
        "data": {
            "plugin_version": "1.0.0",
            "engine_version": "5.6",
            "project_name": "CortexSandbox",
        }
    }
    router = make_router("core", connection, "core docs")
    editors = [
        _editor(8742, 1000, "2026-01-01T00:00:00Z"),
        _editor(8743, 2000, "2026-01-01T01:00:00Z"),
    ]

    with patch("cortex_mcp.tools.routers._discover_all_editors", return_value=editors):
        payload = json.loads(router("get_status"))

    assert payload["connected_editor"] == {"pid": 2000, "port": 8743}
    assert len(payload["available_editors"]) == 2
    connection.send_command.assert_called_once_with("get_status")


def test_core_router_uses_cached_send_for_get_data_catalog():
    connection = MagicMock()
    connection.send_command_cached.return_value = {"success": True, "data": {"datatables": []}}

    router = make_router("core", connection, "core docs")
    payload = json.loads(router("get_data_catalog"))

    assert payload["datatables"] == []
    connection.send_command_cached.assert_called_once_with(
        "data.get_data_catalog",
        {},
        ttl=600,
    )
    connection.send_command.assert_not_called()


def test_core_router_dispatches_normal_commands_via_tcp():
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": {"saved": True}}

    router = make_router("core", connection, "core docs")
    payload = json.loads(router("save_asset", {"asset_path": "/Game/Test"}))

    assert payload["saved"] is True
    connection.send_command.assert_called_once_with(
        "core.save_asset",
        {"asset_path": "/Game/Test"},
    )


def test_register_router_tools_registers_all_domains():
    mcp = MockMCP()
    connection = MagicMock()
    docstrings = {domain: f"{domain} docs" for domain in DOMAINS}

    register_router_tools(mcp, connection, docstrings)

    assert set(mcp.tools) == {f"{domain}_cmd" for domain in DOMAINS}
    assert mcp.tools["core_cmd"]["description"] == "core docs"
