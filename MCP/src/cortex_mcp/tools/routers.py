"""Consolidated domain router tools."""

from __future__ import annotations

import json
import logging
from typing import Annotated, Any, Callable

from mcp.server.fastmcp.utilities.func_metadata import ArgModelBase
from pydantic import ConfigDict, WithJsonSchema, create_model

from cortex_mcp.capabilities import CORE_DOMAINS
from cortex_mcp.pagination import PaginationCache, decode_cursor
from cortex_mcp.response import format_response, _find_largest_list
from cortex_mcp.schema_generator import (
    SCHEMA_VERSION,
    get_schema_dir,
    read_meta_from_file,
    read_schema_metadata,
)
from cortex_mcp.tcp_client import _discover_all_editors, _is_editor_alive
from cortex_mcp.tcp_client import UECommandError


logger = logging.getLogger(__name__)
_TTL_CATALOG = 600
_CACHED_READ_TTL = 300

_pagination_cache = PaginationCache(max_entries=5, ttl_seconds=60.0)
_CACHED_READ_COMMANDS = {
    ("blueprint", "list_scs_components"),
    ("blueprint", "list_inherited_properties"),
    ("blueprint", "list_settable_defaults"),
    ("graph", "list_event_handlers"),
    ("level", "list_actor_classes"),
}

_MAX_LIMIT = 200
_UE_ERROR_RESERVED_FIELDS = {"success", "_error", "_message", "_command"}


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


def _should_forward_limit_to_cpp(domain: str, command: str, params: dict) -> bool:
    """Some C++ commands implement domain-specific limit semantics and must receive it."""
    if domain == "data" and command == "search_datatable_content":
        return params.get("search_mode") == "string_table_refs"
    if domain == "editor" and command == "list_cvars":
        return True
    if domain == "anim" and command == "list_assets":
        return True
    return False


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
    return format_response(payload, "ue_command_error")


_CANONICAL_ROUTER_SHAPE = {"command": "string", "params": "object"}


class _StrictRouterArguments(ArgModelBase):
    model_config = ConfigDict(arbitrary_types_allowed=True, extra="allow")

    def model_dump_one_level(self) -> dict[str, Any]:
        arguments = super().model_dump_one_level()
        arguments.update(self.model_extra or {})
        return arguments


def _invalid_invocation_shape(message: str) -> str:
    return json.dumps({
        "_error": "INVALID_INVOCATION_SHAPE",
        "_message": message,
        "canonical_shape": _CANONICAL_ROUTER_SHAPE,
    })


def _batch_has_zero_commands(params) -> bool:
    if not isinstance(params, dict):
        return True
    commands = params.get("commands")
    if commands is None:
        commands = params.get("steps")
    if commands is None:
        return True
    return not isinstance(commands, list) or len(commands) == 0


def strict_router_tool(router, domain: str) -> Callable[[str, dict | None], str]:
    """Wrap a domain router with the strict {command, params} envelope contract."""

    def wrapped(command: str, params: dict | None = None, **_extra) -> str:
        if _extra:
            return _invalid_invocation_shape(
                f"Malformed {domain}_cmd envelope: unexpected top-level operation fields "
                f"{sorted(_extra)}. Pass all operation fields inside the params object."
            )
        if params is not None and not isinstance(params, dict):
            return _invalid_invocation_shape(
                f"Malformed {domain}_cmd envelope: params must be an object, got {type(params).__name__}."
            )
        if domain == "core" and command in {"batch_query", "batch"} and _batch_has_zero_commands(params):
            return _invalid_invocation_shape(
                "batch_query requires at least one command; zero-command batches are never successful."
            )
        return router(command, params)

    wrapped.__name__ = f"{domain}_cmd"
    wrapped.__doc__ = router.__doc__
    return wrapped


def make_router(domain: str, connection, docstring: str) -> Callable[[str, dict | None], str]:
    """Create a single router tool function for a domain."""

    def router(command: str, params: dict | None = None) -> str:
        route_params = params or {}
        qualified = _qualify_command(domain, command)
        record_tool = getattr(connection, "record_tool_invocation", None)
        if callable(record_tool):
            record_tool(
                f"{domain}_cmd",
                qualified,
                parallel=bool(route_params.get("_telemetry_parallel")),
            )

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
                    commands = route_params.get("commands")
                    if commands is None:
                        commands = route_params.get("steps")
                    if isinstance(commands, str):
                        commands = _json.loads(commands)
                    batch_params = {"commands": commands}
                    for key in ("stop_on_error", "rollback_on_error", "verify_rollback"):
                        if key in route_params:
                            batch_params[key] = route_params[key]
                    response = connection.send_command("batch", batch_params)
                    return format_response(response.get("data", {}), "batch_query")

            # UMG animation binding inspection and guarded removal
            if domain == "umg" and command in {"remove_animation_binding", "list_animation_bindings"}:
                if command == "remove_animation_binding":
                    if any(route_params.get(k) is not None for k in ("limit", "cursor", "offset")):
                        return json.dumps({
                            "_error": "INVALID_FIELD",
                            "_message": "Pagination parameters (limit, cursor, offset) are not supported on remove_animation_binding.",
                        })
                response = connection.send_command(qualified, route_params)
                return format_response(response.get("data", {}), f"{domain}_cmd")

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
                if _should_forward_limit_to_cpp(domain, command, route_params):
                    response = connection.send_command(qualified, route_params)
                    return format_response(response.get("data", {}), f"{domain}_cmd")
                return _handle_limit_request(domain, command, route_params, limit, connection)

            # No pagination — normal dispatch
            if (domain, command) in _CACHED_READ_COMMANDS:
                response = connection.send_command_cached(
                    qualified,
                    route_params,
                    ttl=_CACHED_READ_TTL,
                )
                return format_response(response.get("data", {}), f"{domain}_cmd")

            response = connection.send_command(qualified, route_params)
            return format_response(response.get("data", {}), f"{domain}_cmd")
        except ConnectionError as exc:
            return f"Error: {exc}"
        except UECommandError as exc:
            return _format_ue_command_error(exc)
        except (RuntimeError, ValueError, KeyError) as exc:
            return f"Error: {exc}"

    router.__name__ = f"{domain}_cmd"
    router.__doc__ = docstring
    return router


def register_router_tools(mcp, connection, docstrings: dict[str, str], domains: tuple[str, ...] = CORE_DOMAINS) -> None:
    """Register one explicit router tool per domain."""
    for domain in domains:
        router = strict_router_tool(make_router(domain, connection, docstrings.get(domain, "")), domain)
        _register_strict_router(mcp, domain, router)


def _register_strict_router(mcp, domain: str, strict_router) -> None:
    """Register the strict wrapper behind a FastMCP-safe (command, params) facade.

    FastMCP refuses tool parameters whose names start with an underscore, so the
    strict wrapper's `**_extra` rejection hook cannot be registered directly.
    The facade keeps the canonical {command, params} envelope exposed to callers.
    """
    docstring = strict_router.__doc__ or ""

    def registered(command: str, params: dict | None = None, **extra) -> str:
        return strict_router(command, params, **extra)

    registered.__name__ = f"{domain}_cmd"
    registered.__doc__ = docstring
    mcp.tool(name=f"{domain}_cmd", description=docstring)(registered)

    tool_manager = getattr(mcp, "_tool_manager", None)
    tool = tool_manager.get_tool(f"{domain}_cmd") if tool_manager is not None else None
    if tool is not None:
        argument_model = create_model(
            f"{domain.title()}RouterArguments",
            __base__=_StrictRouterArguments,
            command=(str, ...),
            params=(Annotated[Any, WithJsonSchema({"type": "object"})], None),
        )
        tool.fn_metadata.arg_model = argument_model
        tool.parameters = argument_model.model_json_schema(by_alias=True)


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
    connected = next((editor for editor in editors if editor.port == connection.port), None)
    if connected is not None:
        data["connected_editor"] = {"pid": connected.pid, "port": connected.port}
    else:
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

    file_timestamps = read_schema_metadata(schema_dir).get("files", {})

    def generated_time(md_file) -> str:
        relative_path = md_file.relative_to(schema_dir).as_posix()
        return file_timestamps.get(relative_path, "unknown")

    domains = {}
    for md_file in sorted(schema_dir.glob("*.md")):
        if md_file.name.startswith("_") or md_file.name == "README.md":
            continue
        meta = read_meta_from_file(md_file)
        domain_name = md_file.stem
        if meta:
            version = int(meta.get("schema_version", "0"))
            domains[domain_name] = {
                "file": md_file.name,
                "generated": generated_time(md_file),
                "schema_version": version,
                "version_current": version == SCHEMA_VERSION,
            }
        else:
            domains[domain_name] = {
                "file": md_file.name,
                "generated": "unknown",
                "error": "No meta block found",
            }

    for subdir in sorted(schema_dir.iterdir()):
        if not subdir.is_dir() or subdir.name.startswith("_"):
            continue
        files = {}
        generated_values = []
        version = 0
        first_file_seen = False
        for md_file in sorted(subdir.glob("*.md")):
            meta = read_meta_from_file(md_file)
            if meta:
                generated = generated_time(md_file)
                files[md_file.name] = generated
                if generated != "unknown":
                    generated_values.append(generated)
                if not first_file_seen:
                    version = int(meta.get("schema_version", "0"))
                    first_file_seen = True
        if files:
            domains[subdir.name] = {
                "files": files,
                "generated": min(generated_values) if generated_values else "unknown",
                "schema_version": version,
                "version_current": version == SCHEMA_VERSION,
            }

    catalog_file = schema_dir / "_catalog.md"
    return json.dumps(
        {
            "exists": True,
            "schema_dir": str(schema_dir),
            "catalog": {
                "generated": generated_time(catalog_file) if catalog_file.exists() else "missing",
            },
            "domains": domains,
            "current_schema_version": SCHEMA_VERSION,
        },
        indent=2,
    )
