"""MCP tool to switch which Unreal Editor instance the MCP server targets."""

import json
import logging

from cortex_mcp.tcp_client import UEConnection, _discover_all_editors, _is_editor_alive

logger = logging.getLogger(__name__)


def register_switch_editor_tools(mcp, connection: UEConnection):
    """Register switch_editor MCP tool."""

    @mcp.tool()
    def switch_editor(pid: int | None = None) -> str:
        """Switch which Unreal Editor instance this MCP server targets."""
        editors = _discover_all_editors()
        if not editors:
            return json.dumps({"error": "No live editors found"})

        if pid is not None:
            if not _is_editor_alive(pid):
                return json.dumps(
                    {
                        "error": "EDITOR_NOT_FOUND",
                        "message": f"PID {pid} is not a live Unreal Editor",
                    }
                )
            target = None
            for editor in editors:
                if editor.pid == pid:
                    target = editor
                    break
            if target is None:
                return json.dumps(
                    {
                        "error": "EDITOR_NOT_FOUND",
                        "message": f"No port file found for PID {pid}",
                    }
                )
        else:
            editors.sort(key=lambda editor: editor.started_at, reverse=True)
            target = editors[0]

        connection.disconnect()
        connection.port = target.port
        connection._pid = target.pid
        connection._project_path = None
        logger.info("Switched to editor PID %d on port %d", target.pid, target.port)

        return json.dumps(
            {
                "port": target.port,
                "pid": target.pid,
                "started_at": target.started_at,
                "message": (
                    f"Now targeting editor PID {target.pid} on port {target.port}"
                ),
            }
        )
