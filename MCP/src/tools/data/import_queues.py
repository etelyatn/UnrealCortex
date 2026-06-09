"""Standalone MCP tools for data import operation queues."""

from __future__ import annotations

import json

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UECommandError

_UE_ERROR_RESERVED_FIELDS = {"success", "_error", "_message", "_command"}


def _format_ue_command_error(exc: UECommandError) -> str:
    payload = {
        "success": False,
        "_error": exc.code,
        "_message": exc.message,
        "_command": exc.command,
    }
    for key, value in exc.details.items():
        if key not in _UE_ERROR_RESERVED_FIELDS:
            payload[key] = value
    return format_response(payload, "apply_import_ops_json")


def register_import_queue_tools(mcp, connection) -> None:
    """Register standalone data import queue tools."""

    @mcp.tool()
    def apply_import_ops_json(
        ops_path: str,
        report_path: str,
        dry_run: bool = True,
        apply: bool = False,
        stop_on_error: bool = True,
        query_back: bool = True,
        allow_partial: bool = False,
    ) -> str:
        """Apply a validated data import operation queue from a JSON file."""
        try:
            response = connection.send_command(
                "data.apply_import_ops_json",
                {
                    "ops_path": ops_path,
                    "report_path": report_path,
                    "dry_run": dry_run,
                    "apply": apply,
                    "stop_on_error": stop_on_error,
                    "query_back": query_back,
                    "allow_partial": allow_partial,
                },
            )
            return format_response(response.get("data", {}), "apply_import_ops_json")
        except UECommandError as exc:
            return _format_ue_command_error(exc)
        except ConnectionError as exc:
            return f"Error: {exc}"
