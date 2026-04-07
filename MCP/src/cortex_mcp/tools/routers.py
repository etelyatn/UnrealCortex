"""Consolidated domain router tools."""

from __future__ import annotations

import json
import logging
from typing import Callable

from cortex_mcp.capabilities import CORE_DOMAINS
from cortex_mcp.pagination import PaginationCache, decode_cursor
from cortex_mcp.response import format_response, _find_largest_list
from cortex_mcp.schema_generator import (
    SCHEMA_VERSION,
    get_schema_dir,
    read_meta_from_file,
)
from cortex_mcp.tcp_client import _discover_all_editors, _is_editor_alive


logger = logging.getLogger(__name__)
_TTL_CATALOG = 600

_pagination_cache = PaginationCache(max_entries=5, ttl_seconds=60.0)

_MAX_LIMIT = 200


def _validate_limit(limit) -> tuple[int | None, str | None]:
    """Validate and coerce the limit parameter. Returns (limit, error_json) — one is always None."""
    try:
        limit = int(limit)
    except (TypeError, ValueError):
        return None, json.dumps({"_error": "INVALID_LIMIT", "_message": f"limit must be an integer between 1 and {_MAX_LIMIT}."})
    if limit < 1 or limit > _MAX_LIMIT:
        return None, json.dumps({"_error": "INVALID_LIMIT", "_message": f"limit must be between 1 and {_MAX_LIMIT}."})
    return limit, None


def _handle_cursor_request(cursor_token: str) -> str:
    """Handle a request that carries a cursor (subsequent page)."""
    try:
        decoded = decode_cursor(cursor_token)
    except ValueError:
        return json.dumps({"_error": "INVALID_CURSOR", "_message": "Cursor is malformed. Pass a cursor value returned from a previous response."})

    key = decoded["key"]
    offset = decoded["offset"]
    limit = decoded["limit"]  # embedded in cursor

    try:
        page, meta = _pagination_cache.get_page(key, offset, limit)
        response = _pagination_cache.rebuild_response(key, page, meta)
    except KeyError:
        return json.dumps({"_error": "CURSOR_EXPIRED", "_message": "Cached results have expired. Re-send the original command with 'limit' to start a new pagination sequence."})

    return format_response(response, "paginated")


def _handle_limit_request(domain: str, command: str, params: dict, limit: int, connection) -> str:
    """Handle a request with limit (first page or re-request)."""
    # Strip limit/cursor from params sent to C++
    clean_params = {k: v for k, v in params.items() if k not in ("limit", "cursor")}
    qualified = _qualify_command(domain, command)

    response = connection.send_command(qualified, clean_params)
    data = response.get("data", {})

    array_key = _find_largest_list(data)
    if array_key is None:
        # No qualifying array — return as-is, limit is a no-op
        return format_response(data, f"{domain}_cmd")

    full_list = data[array_key]
    template = {k: v for k, v in data.items() if k != array_key}

    cache_key = _pagination_cache.store(qualified, clean_params, array_key, full_list, template)

    try:
        page, meta = _pagination_cache.get_page(cache_key, offset=0, limit=limit)
        result = _pagination_cache.rebuild_response(cache_key, page, meta)
    except KeyError:
        return json.dumps({"_error": "CURSOR_EXPIRED", "_message": "Cached results have expired. Re-send the original command with 'limit' to start a new pagination sequence."})
    return format_response(result, f"{domain}_cmd")


def make_router(domain: str, connection, docstring: str) -> Callable[[str, dict | None], str]:
    """Create a single router tool function for a domain."""

    def router(command: str, params: dict | None = None) -> str:
        route_params = params or {}

        try:
            # Handle core special commands first (no pagination for these)
            if domain == "core":
                if command == "switch_editor":
                    return _switch_editor(connection, route_params)
                if command == "schema_status":
                    return _schema_status()
                if command == "get_status":
                    return _get_status(connection)
                if command == "get_data_catalog":
                    response = connection.send_command_cached(
                        "data.get_data_catalog",
                        route_params,
                        ttl=_TTL_CATALOG,
                    )
                    return format_response(response.get("data", {}), "get_data_catalog")
                if command == "batch_query":
                    import json as _json
                    commands = route_params.get("commands", [])
                    if isinstance(commands, str):
                        commands = _json.loads(commands)
                    response = connection.send_command("batch", {"commands": commands})
                    return format_response(response.get("data", {}), "batch_query")

            # Check for cursor (subsequent page — no C++ call needed)
            cursor_token = route_params.get("cursor")
            if cursor_token is not None:
                return _handle_cursor_request(cursor_token)

            # Check for limit (first page)
            limit_param = route_params.get("limit")
            if limit_param is not None:
                limit, error = _validate_limit(limit_param)
                if error:
                    return error
                return _handle_limit_request(domain, command, route_params, limit, connection)

            # No pagination — normal dispatch
            response = connection.send_command(_qualify_command(domain, command), route_params)
            return format_response(response.get("data", {}), f"{domain}_cmd")
        except ConnectionError as exc:
            return f"Error: {exc}"
        except (RuntimeError, ValueError, KeyError) as exc:
            return f"Error: {exc}"

    router.__name__ = f"{domain}_cmd"
    router.__doc__ = docstring
    return router


def register_router_tools(mcp, connection, docstrings: dict[str, str], domains: tuple[str, ...] = CORE_DOMAINS) -> None:
    """Register one explicit router tool per domain."""
    for domain in domains:
        router = make_router(domain, connection, docstrings.get(domain, ""))
        mcp.tool(name=f"{domain}_cmd", description=router.__doc__)(router)


def _qualify_command(domain: str, command: str) -> str:
    if domain == "core" and command in {"get_status", "get_capabilities"}:
        return command
    return f"{domain}.{command}"


def _switch_editor(connection, params: dict) -> str:
    editors = _discover_all_editors()
    if not editors:
        return json.dumps({"error": "No live editors found"})

    pid = params.get("pid")
    if pid is not None:
        pid = int(pid)
        if not _is_editor_alive(pid):
            return json.dumps(
                {
                    "error": "EDITOR_NOT_FOUND",
                    "message": f"PID {pid} is not a live Unreal Editor",
                }
            )
        target = next((editor for editor in editors if editor.pid == pid), None)
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
            "message": f"Now targeting editor PID {target.pid} on port {target.port}",
        }
    )


def _get_status(connection) -> str:
    from cortex_mcp.schema_generator import _decode_data

    response = connection.send_command("get_status")
    data = _decode_data(response)

    editors = _discover_all_editors()
    data["connected_editor"] = {"pid": connection._pid, "port": connection.port}
    data["available_editors"] = [
        {"pid": editor.pid, "port": editor.port, "started_at": editor.started_at}
        for editor in editors
    ]
    return format_response(data, "get_status")


def _schema_status() -> str:
    try:
        schema_dir = get_schema_dir()
    except FileNotFoundError:
        return json.dumps({"error": "Cannot find project root"})

    if not schema_dir.exists():
        return json.dumps(
            {
                "exists": False,
                "suggestion": "Run generate_project_schema to create schema files.",
            }
        )

    domains = {}
    for md_file in schema_dir.glob("*.md"):
        if md_file.name.startswith("_") or md_file.name == "README.md":
            continue
        meta = read_meta_from_file(md_file)
        domain_name = md_file.stem
        if meta:
            version = int(meta.get("schema_version", "0"))
            domains[domain_name] = {
                "file": md_file.name,
                "generated": meta.get("generated", "unknown"),
                "schema_version": version,
                "version_current": version == SCHEMA_VERSION,
            }
        else:
            domains[domain_name] = {
                "file": md_file.name,
                "generated": "unknown",
                "error": "No meta block found",
            }

    for subdir in schema_dir.iterdir():
        if not subdir.is_dir() or subdir.name.startswith("_"):
            continue
        files = {}
        oldest_generated = None
        version = 0
        first_file_seen = False
        for md_file in sorted(subdir.glob("*.md")):
            meta = read_meta_from_file(md_file)
            if meta:
                files[md_file.name] = meta.get("generated", "unknown")
                generated = meta.get("generated")
                if oldest_generated is None or (generated and generated < oldest_generated):
                    oldest_generated = generated
                if not first_file_seen:
                    version = int(meta.get("schema_version", "0"))
                    first_file_seen = True
        if files:
            domains[subdir.name] = {
                "files": files,
                "generated": oldest_generated or "unknown",
                "schema_version": version,
                "version_current": version == SCHEMA_VERSION,
            }

    catalog_meta = read_meta_from_file(schema_dir / "_catalog.md")
    return json.dumps(
        {
            "exists": True,
            "schema_dir": str(schema_dir),
            "catalog": {
                "generated": catalog_meta.get("generated", "unknown") if catalog_meta else "missing",
            },
            "domains": domains,
            "current_schema_version": SCHEMA_VERSION,
        },
        indent=2,
    )
