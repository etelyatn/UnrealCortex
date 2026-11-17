"""Standalone MCP tools for data import operation queues."""

from __future__ import annotations

import json

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UECommandError
from cortex_mcp.tool_output import CONNECTION_ERROR_CODE, canonical_failure


def _format_ue_command_error(exc: UECommandError) -> str:
    payload = canonical_failure(exc.code, exc.message, exc.details)
    payload["_command"] = exc.command
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
            return format_response(
                canonical_failure(CONNECTION_ERROR_CODE, str(exc)),
                "apply_import_ops_json",
            )
