"""Consolidated domain router tools."""

from __future__ import annotations

import json
import logging
import math
from typing import Annotated, Any, Callable

from mcp.server.fastmcp.utilities.func_metadata import ArgModelBase
from pydantic import ConfigDict, WithJsonSchema, create_model

from cortex_mcp.capabilities import CORE_DOMAINS
from cortex_mcp.graph_patch_boundary import dispatch_graph_apply_patch
from cortex_mcp.pagination import PaginationCache, decode_cursor, encode_cursor
from cortex_mcp.replay_boundary import _finalize, dispatch_replay_command
from cortex_mcp.response import (
    ANIMATION_AUTHORING_COMMANDS,
    MAX_RESPONSE_CHARS,
    compact_json,
    format_native_page,
    format_response,
)
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

# Deferred editor commands answer after the native work completes, which can outlast the 60 s default
# socket window: wait_ready waits up to its own `timeout` (default 120 s); live_coding_compile blocks the
# editor until the patch is built (native transport bound 1800 s). compile_all and export_class_instances
# are synchronous natively and scale with the project (a 829-Blueprint compile_all measured 177 s), so
# they get a long fixed window; the whole-project reads export_migration_analysis, export_dependency_graph,
# find_duplicate_graphs and find_name_lookups load every Blueprint of their prefixes and share it;
# find_interface_implementers and find_type_usages load every Blueprint that implements or references the type;
# data.plan_type_replacement classifies the referencers of a user type and exports a baseline of every affected
# DataTable/DataAsset, and data.verify_type_replacement loads, compiles, diffs and optionally resaves all of them.
_WAIT_READY_DEFAULT_SECONDS = 120.0
_DEFERRED_TIMEOUT_MARGIN = 30.0
_LIVE_CODING_COMPILE_WINDOW = 1830.0
_LONG_SYNCHRONOUS_WINDOW = 1800.0
_LONG_SYNCHRONOUS_COMMANDS = frozenset({
    "data.plan_type_replacement",
    "data.verify_type_replacement",
    "blueprint.compile_all",
    "blueprint.export_migration_analysis",
    "level.export_class_instances",
    "reflect.export_dependency_graph",
    "reflect.find_duplicate_graphs",
    "reflect.find_interface_implementers",
    "reflect.find_name_lookups",
    "reflect.find_type_usages",
})


def _dispatch_timeout(qualified: str, params: dict) -> float | None:
    """Per-command receive window in seconds, or None to keep the connection default."""
    if qualified == "editor.live_coding_compile":
        return _LIVE_CODING_COMPILE_WINDOW
    if qualified in _LONG_SYNCHRONOUS_COMMANDS:
        return _LONG_SYNCHRONOUS_WINDOW
    if qualified == "editor.wait_ready":
        requested = params.get("timeout")
        if (
            isinstance(requested, bool)
            or not isinstance(requested, (int, float))
            or not math.isfinite(requested)
        ):
            requested = _WAIT_READY_DEFAULT_SECONDS
        return float(requested) + _DEFERRED_TIMEOUT_MARGIN
    return None


_pagination_cache = PaginationCache(max_entries=16, ttl_seconds=600.0)
_CACHED_READ_COMMANDS = {
    ("blueprint", "list_scs_components"),
    ("blueprint", "list_inherited_properties"),
    ("blueprint", "list_settable_defaults"),
    ("graph", "list_event_handlers"),
    ("level", "list_actor_classes"),
}

# Native-owned reads consume limit/offset themselves. Their window is authoritative: a native
# limit above the Python cache maximum, or the native limit=0 "unlimited" sentinel, is forwarded
# unchanged and never reinterpreted as a Python pagination request. No Python cursor is ever added
# to a native page, and an oversized native page refuses instead of being shortened.
NATIVE_LIMIT_COMMANDS = frozenset({
    ("data", "query_datatable"),
    ("data", "search_datatable_content"),
    ("data", "search_assets"),
    ("level", "list_actors"),
    ("reflect", "find_overrides"),
    ("reflect", "find_usages"),
    ("reflect", "get_dependencies"),
    ("reflect", "get_referencers"),
    ("reflect", "search"),
    ("statetree", "list_assets"),
    ("editor", "list_cvars"),
    ("editor", "get_recent_logs"),
    ("anim", "list_assets"),
    ("gen", "list_jobs"),
    # umg.list_animation_bindings declares limit/offset, validates them in C++ and emits its own
    # next_offset/is_complete continuation, so it owns its page like the other native readers.
    ("umg", "list_animation_bindings"),
})

# Cache-owned unbounded reads: qualified read command -> the owner's primary array. Only simple
# unbounded arrays appear here, with identities verified in the owner source. Native-owned
# commands, mixed object/edge reads, hierarchical trees, catalogs and object reads never enter the
# mapping, so no read is guessed into cached pagination.
CACHED_PAGE_ARRAYS = {
    "data.list_datatables": "datatables",
    "data.list_data_assets": "data_assets",
    "data.list_curve_tables": "curve_tables",
    "data.list_string_tables": "string_tables",
    "data.list_gameplay_tags": "tags",
    "level.find_actors": "matches",
    "level.list_actor_classes": "classes",
    "level.list_component_classes": "classes",
    "graph.list_graphs": "graphs",
    "graph.search_nodes": "results",
    "graph.list_event_handlers": "nodes",
    "graph.find_event_handler": "nodes",
    "graph.find_function_calls": "nodes",
    "blueprint.list": "blueprints",
    "blueprint.list_scs_components": "components",
    "material.list_materials": "materials",
    "material.list_instances": "instances",
    "material.list_collections": "collections",
    "material.list_nodes": "nodes",
    # core.batch_query executes once; its step results are the paged array.
    "core.batch_query": "results",
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


_BATCH_QUERY_COMMAND = "core.batch_query"


def _cursor_expired(qualified: str | None = None) -> str:
    if qualified == _BATCH_QUERY_COMMAND:
        # A batch is not a re-runnable read: its steps already executed (and may have changed state).
        message = (
            "This batch already executed and its stored results are no longer available "
            "(the snapshot expired or was evicted). Do NOT re-send the batch: that would execute its "
            "steps again. Check the editor state or run read-only queries to observe the outcome."
        )
    else:
        message = "Cached results have expired. Re-send the original command with 'limit' to start a new pagination sequence."
    return json.dumps({"_error": "CURSOR_EXPIRED", "_message": message})


def _invalid_cursor(message: str) -> str:
    return json.dumps({"_error": "INVALID_CURSOR", "_message": message})


def _cached_refusal(array_key: str) -> str:
    return compact_json({
        "_error": "RESPONSE_TOO_LARGE",
        "_message": (
            f"No entry of '{array_key}' fits the {MAX_RESPONSE_CHARS}-character response budget; "
            "the page was refused."
        ),
        "reader_complete": False,
        "_suggestion": "Reduce the per-entry fields, or query with a smaller scope.",
    })


def _batch_oversized_step_refusal(
    qualified: str,
    array_key: str,
    cache_key: str,
    offset: int,
    limit: int,
    total: int,
) -> str:
    """Refusal for one executed batch step that alone exceeds the budget.

    The batch already ran (its steps may have mutated state), so the refusal says so, names the
    snapshot and the oversized step, and carries a cursor positioned after that step so the
    remaining step results stay retrievable without re-executing anything.
    """
    snapshot_id = _pagination_cache.snapshot_id(cache_key)
    next_offset = offset + 1
    has_more = next_offset < total
    next_cursor = (
        encode_cursor(
            cache_key, next_offset, limit,
            command=qualified, array=array_key, snapshot_id=snapshot_id,
        )
        if has_more
        else None
    )
    return compact_json({
        "_error": "RESPONSE_TOO_LARGE",
        "_message": (
            f"Batch step at index {offset} alone exceeds the {MAX_RESPONSE_CHARS}-character "
            "response budget and was not returned. The batch already executed (its steps may have "
            "changed state); do not re-run it."
        ),
        "reader_complete": False,
        "snapshot_id": snapshot_id,
        "oversized_step_index": offset,
        "_pagination": {
            "returned": 0,
            "total": total,
            "has_more": has_more,
            "snapshot_id": snapshot_id,
            "next_cursor": next_cursor,
        },
        "_suggestion": (
            "Call core.batch_query with cursor=_pagination.next_cursor to read the remaining step "
            "results from the stored snapshot; this never re-executes the batch."
            if has_more
            else "No further step results remain in the snapshot; do not re-run the batch."
        ),
    })


def _format_cached_page(
    qualified: str,
    array_key: str,
    cache_key: str,
    offset: int,
    limit: int,
    page: list,
    total: int,
) -> str:
    """Emit the largest prefix of a cached page that fits the budget *including* its metadata.

    ``returned`` and the continuation are computed from the rows actually emitted, so a bounded
    page can never skip the rows it omitted. An exhausted snapshot (the offset is past the end) is
    a normal terminal empty page — zero rows, no continuation, verified against the real template
    budget — and a zero-row page never reports ``has_more``. An empty slice that is *not* exhausted
    (only reachable from a malformed cursor) refuses instead of claiming the read is complete, and
    if not even one row of a non-empty page fits it refuses likewise.
    """
    snapshot_id = _pagination_cache.snapshot_id(cache_key)
    if not page:
        if offset >= total:
            meta = {
                "returned": 0,
                "total": total,
                "has_more": False,
                "snapshot_id": snapshot_id,
                "next_cursor": None,
            }
            text = compact_json(_pagination_cache.rebuild_response(cache_key, [], meta))
            return text if len(text) <= MAX_RESPONSE_CHARS else _cached_refusal(array_key)
        return _cached_refusal(array_key)

    for count in range(len(page), 0, -1):
        next_offset = offset + count
        has_more = next_offset < total
        meta = {
            "returned": count,
            "total": total,
            "has_more": has_more,
            "snapshot_id": snapshot_id,
            "next_cursor": (
                encode_cursor(
                    cache_key, next_offset, limit,
                    command=qualified, array=array_key, snapshot_id=snapshot_id,
                )
                if has_more
                else None
            ),
        }
        text = compact_json(_pagination_cache.rebuild_response(cache_key, page[:count], meta))
        if len(text) <= MAX_RESPONSE_CHARS:
            return text
    if qualified == "core.batch_query":
        return _batch_oversized_step_refusal(qualified, array_key, cache_key, offset, limit, total)
    return _cached_refusal(array_key)


def _handle_cursor_request(cursor_token: str, qualified: str) -> str:
    """Handle a subsequent page for the cache-owned read ``qualified``.

    A cursor must agree with the route mapping **and** with the provenance stored for the snapshot
    it addresses: a token re-tagged for another read (or another array) refuses before anything is
    sliced, rebuilt or dispatched, so a foreign payload can never be returned through this read.
    """
    try:
        decoded = decode_cursor(cursor_token)
    except ValueError:
        return _invalid_cursor(
            "Cursor is malformed. Pass a cursor value returned from a previous response."
        )

    if decoded.get("command") != qualified or qualified not in CACHED_PAGE_ARRAYS:
        return _invalid_cursor(
            "Cursor does not belong to this read command. Start a new pagination sequence with 'limit'."
        )

    array_key = CACHED_PAGE_ARRAYS[qualified]
    key = decoded["key"]

    if decoded.get("snapshot_id") is None:
        return _invalid_cursor(
            "Cursor is malformed. Pass a cursor value returned from a previous response."
        )

    provenance = _pagination_cache.entry_provenance(key)
    if provenance is None:
        return _cursor_expired(qualified)
    stored_command, stored_array, stored_snapshot_id = provenance
    if (
        decoded.get("array") != array_key
        or stored_command != qualified
        or stored_array != array_key
        or decoded["snapshot_id"] != stored_snapshot_id
    ):
        return _invalid_cursor(
            "Cursor does not belong to this read command. Start a new pagination sequence with 'limit'."
        )

    offset = decoded["offset"]
    limit = decoded["limit"]  # embedded in cursor; re-validated against the minted contract
    if limit < 1 or limit > _MAX_LIMIT:
        return _invalid_cursor(
            "Cursor is malformed. Pass a cursor value returned from a previous response."
        )

    try:
        page, meta = _pagination_cache.get_page(key, offset, limit)
        return _format_cached_page(qualified, array_key, key, offset, limit, page, meta["total"])
    except KeyError:
        return _cursor_expired(qualified)


def _handle_limit_request(qualified: str, params: dict, limit: int, connection) -> str:
    """Handle the first page of a cache-owned read (or a re-request)."""
    # Strip limit/cursor from params sent to C++
    clean_params = {k: v for k, v in params.items() if k not in ("limit", "cursor")}
    array_key = CACHED_PAGE_ARRAYS[qualified]

    response = connection.send_command(qualified, clean_params)
    data = response.get("data", {})

    full_list = data.get(array_key)
    if not isinstance(full_list, list):
        # The owner did not return its primary array: keep the read complete rather than guess.
        return format_response(
            data,
            f"{qualified.partition('.')[0]}_cmd",
            qualified_command=qualified,
            request_params=params,
        )

    template = {k: v for k, v in data.items() if k != array_key}
    cache_key = _pagination_cache.store(qualified, clean_params, array_key, full_list, template)

    try:
        page, meta = _pagination_cache.get_page(cache_key, offset=0, limit=limit)
        return _format_cached_page(qualified, array_key, cache_key, 0, limit, page, meta["total"])
    except KeyError:
        return _cursor_expired(qualified)


_EXECUTED_BATCH_NOTE = (
    "This batch already executed once (its steps may have changed state); do not re-run it. "
    "The page below holds the first step results; the remaining step results are available by "
    "calling core.batch_query with cursor=_pagination.next_cursor, which is served from the stored "
    "snapshot and never re-executes the batch."
)


def _is_too_large_refusal(text: str) -> bool:
    try:
        payload = json.loads(text)
    except ValueError:
        return False
    return isinstance(payload, dict) and payload.get("_error") == "RESPONSE_TOO_LARGE"


def _batch_query(connection, route_params: dict) -> str:
    """core.batch_query: one native execution, optionally served as cached pages.

    With ``limit`` the single native ``batch`` result is snapshotted and its step results are paged
    from the cache; a ``cursor`` is served from that snapshot and never re-executes the batch (the
    steps may mutate). Without ``limit`` a response that fits is returned whole; an oversized one is
    never discarded or shortened: the executed results are snapshotted and the first page that fits
    is returned with a cursor and a note that the batch already ran.
    """
    qualified = _BATCH_QUERY_COMMAND
    cursor_token = route_params.get("cursor")
    if cursor_token is not None:
        return _handle_cursor_request(cursor_token, qualified)

    limit = None
    if route_params.get("limit") is not None:
        limit, error = _validate_limit(route_params["limit"])
        if error:
            return error

    commands = route_params.get("commands")
    if commands is None:
        commands = route_params.get("steps")
    if isinstance(commands, str):
        commands = json.loads(commands)
    batch_params = {"commands": commands}
    for key in ("stop_on_error", "rollback_on_error", "verify_rollback"):
        if key in route_params:
            batch_params[key] = route_params[key]
    if isinstance(commands, list) and any(
        isinstance(step, dict) and step.get("command") == "graph.apply_patch"
        for step in commands
    ):
        return json.dumps({
            "_error": "INVALID_OPERATION",
            "_message": (
                "graph.apply_patch is a standalone mutation and cannot run inside "
                "core_cmd(batch_query); call graph_cmd or blueprint_compose so the "
                "MCP prune response boundary can run before mutation."
            ),
        })

    response = connection.send_command("batch", batch_params)
    data = response.get("data", {})
    array_key = CACHED_PAGE_ARRAYS[qualified]
    note = None
    if limit is None:
        text = format_response(data, "batch_query")
        results = data.get(array_key)
        if not _is_too_large_refusal(text) or not isinstance(results, list) or not results:
            return text
        # The batch has already executed, so its results are never discarded and the caller is never
        # asked to run it again: the executed results are snapshotted and served from the cache.
        limit = min(len(results), _MAX_LIMIT)
        note = _EXECUTED_BATCH_NOTE

    results = data.get(array_key)
    if not isinstance(results, list):
        return format_response(data, "batch_query")
    template = {k: v for k, v in data.items() if k != array_key}
    if note is not None:
        template["_note"] = note
    cache_key = _pagination_cache.store(qualified, batch_params, array_key, results, template)
    try:
        page, meta = _pagination_cache.get_page(cache_key, offset=0, limit=limit)
        return _format_cached_page(qualified, array_key, cache_key, 0, limit, page, meta["total"])
    except KeyError:
        return _cursor_expired(qualified)


def _error_payload(code: str, message: str, details) -> dict:
    """Standard native error payload with non-reserved detail fields preserved."""
    payload = {
        "success": False,
        "_error": code,
        "_message": message,
    }
    if isinstance(details, dict):
        for key, value in details.items():
            if key not in _UE_ERROR_RESERVED_FIELDS:
                payload[key] = value
    return payload


def _native_failure_payload(response) -> dict | None:
    """Convert a transport-returned native failure envelope into the standard error payload.

    EditorConnection raises UECommandError for failed commands; this keeps any non-raising
    failure envelope from being mistaken for absent data and reported as an empty success.
    """
    if not isinstance(response, dict) or response.get("success") is not False:
        return None
    error = response.get("error")
    if not isinstance(error, dict):
        error = {}
    return _error_payload(
        error.get("code", "UNKNOWN"),
        error.get("message", "Unknown error"),
        error.get("details"),
    )


def _format_ue_command_error(
    exc: UECommandError,
    command: str | None = None,
    *,
    qualified_command: str | None = None,
    request_params: dict | None = None,
) -> str:
    payload = _error_payload(exc.code, exc.message, exc.details)
    payload["_command"] = exc.command
    return format_response(
        payload,
        "ue_command_error",
        command=command,
        qualified_command=qualified_command or exc.command,
        request_params=request_params,
    )


_CANONICAL_ROUTER_SHAPE = {"command": "string", "params": "object"}


class _StrictRouterArguments(ArgModelBase):
    model_config = ConfigDict(arbitrary_types_allowed=True, extra="allow")

    def model_dump_one_level(self) -> dict[str, Any]:
        arguments = super().model_dump_one_level()
        arguments.update(self.model_extra or {})
        return arguments


def _invalid_invocation_shape(
    message: str, *, finalize: Callable[[Any], str] | None = None
) -> str:
    payload = {
        "_error": "INVALID_INVOCATION_SHAPE",
        "_message": message,
        "canonical_shape": _CANONICAL_ROUTER_SHAPE,
    }
    if finalize is not None:
        return finalize(payload)
    return json.dumps(payload)


def _batch_has_zero_commands(params) -> bool:
    if not isinstance(params, dict):
        return True
    if params.get("cursor") is not None:
        return False  # a cursor page is served from its snapshot and carries no commands
    commands = params.get("commands")
    if commands is None:
        commands = params.get("steps")
    if commands is None:
        return True
    return not isinstance(commands, list) or len(commands) == 0


def _batch_contains_replay_start(route_params: dict) -> bool:
    """True when a core batch carries a one-shot replay start step.

    Replay start must be dispatched directly; a generic batch may retry, so the
    start is refused before any run/request is allocated (matching the native
    ``FCortexCommandRouter::IsInBatch`` denial).
    """
    commands = route_params.get("commands")
    if commands is None:
        commands = route_params.get("steps")
    if isinstance(commands, str):
        try:
            commands = json.loads(commands)
        except (TypeError, ValueError):
            return False
    if not isinstance(commands, list):
        return False
    return any(
        isinstance(step, dict) and step.get("command") == "replay.start_replay"
        for step in commands
    )


def strict_router_tool(router, domain: str) -> Callable[[str, dict | None], str]:
    """Wrap a domain router with the strict {command, params} envelope contract."""

    # Replay owns a 39,000-byte encoded-UTF-8 budget for every reply path, including
    # pre-dispatch envelope rejections that never reach dispatch_replay_command.
    envelope_finalize = _finalize if domain == "replay" else None

    def reject(message: str) -> str:
        return _invalid_invocation_shape(message, finalize=envelope_finalize)

    def wrapped(command: str, params: dict | None = None, **_extra) -> str:
        if _extra:
            return reject(
                f"Malformed {domain}_cmd envelope: unexpected top-level operation fields "
                f"{sorted(_extra)}. Pass all operation fields inside the params object."
            )
        if params is not None and not isinstance(params, dict):
            return reject(
                f"Malformed {domain}_cmd envelope: params must be an object, got {type(params).__name__}."
            )
        if domain == "core" and command in {"batch_query", "batch"} and _batch_has_zero_commands(params):
            return reject(
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
            # Replay start must be dispatched directly; refuse it inside a core batch before sending.
            if (
                domain == "core"
                and command in {"batch", "batch_query"}
                and _batch_contains_replay_start(route_params)
            ):
                return json.dumps({
                    "_error": "INVALID_OPERATION",
                    "_message": (
                        "replay.start_replay is a one-shot operation and cannot run inside "
                        "core_cmd(batch/batch_query); call replay_cmd directly so the MCP "
                        "one-shot boundary runs before dispatch."
                    ),
                })

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
                    return format_response(
                        response.get("data", {}),
                        "get_data_catalog",
                        qualified_command="get_data_catalog",
                        request_params=route_params,
                    )
                if command == "get_capabilities":
                    return _get_capabilities(connection, route_params)
                if command == "batch_query":
                    return _batch_query(connection, route_params)

            if domain == "graph" and command == "apply_patch":
                return dispatch_graph_apply_patch(
                    connection,
                    route_params,
                    tool_name="graph_cmd",
                )

            # Replay: validate and dispatch before the generic cursor/cache shortcuts.
            if domain == "replay":
                return dispatch_replay_command(connection, command, route_params)

            # Property-binding reads/writes must never be interpreted as cached pagination.
            property_binding_write = domain == "umg" and command == "set_property_binding"
            property_binding_read_command = domain == "umg" and command in {"get_tree", "get_widget"}
            if (
                property_binding_read_command
                and "include_property_bindings" in route_params
                and not isinstance(route_params["include_property_bindings"], bool)
            ):
                return json.dumps({
                    "_error": "INVALID_FIELD",
                    "_message": "include_property_bindings must be boolean.",
                })
            property_binding_read = (
                property_binding_read_command
                and route_params.get("include_property_bindings") is True
            )
            if property_binding_write or property_binding_read:
                if any(key in route_params for key in ("limit", "cursor", "offset")):
                    return json.dumps({
                        "_error": "INVALID_FIELD",
                        "_message": "Pagination fields are unsupported for property binding operations.",
                    })
                response = connection.send_command(qualified, route_params)
                return format_response(response.get("data", {}), f"{domain}_cmd")

            # Animation authoring writes dispatch directly: any supplied cursor/limit/offset
            # field refuses before generic cached pagination, and native failures are
            # propagated instead of being reported as an empty success.
            if domain == "umg" and command in ANIMATION_AUTHORING_COMMANDS:
                if any(key in route_params for key in ("limit", "cursor", "offset")):
                    return json.dumps({
                        "_error": "INVALID_FIELD",
                        "_message": "Pagination fields are unsupported for animation authoring.",
                    })
                response = connection.send_command(qualified, route_params)
                failure = _native_failure_payload(response)
                if failure is not None:
                    return format_response(failure, "ue_command_error", command=command)
                return format_response(response.get("data", {}), f"{domain}_cmd", command=command)

            # UMG animation binding inspection and guarded removal
            if domain == "umg" and command in {"remove_animation_binding", "list_animation_bindings"}:
                if command == "remove_animation_binding":
                    if any(route_params.get(k) is not None for k in ("limit", "cursor", "offset")):
                        return json.dumps({
                            "_error": "INVALID_FIELD",
                            "_message": "Pagination parameters (limit, cursor, offset) are not supported on remove_animation_binding.",
                        })
                    detail = False
                else:
                    if (
                        "include_track_content" in route_params
                        and not isinstance(route_params["include_track_content"], bool)
                    ):
                        return json.dumps({
                            "_error": "INVALID_FIELD",
                            "_message": "include_track_content must be boolean.",
                        })
                    detail = route_params.get("include_track_content") is True
                    if route_params.get("cursor") is not None:
                        return _invalid_cursor(
                            f"cursor is unsupported for native-owned {qualified}; "
                            "pass native limit/offset instead."
                        )
                response = connection.send_command(qualified, route_params)
                failure = _native_failure_payload(response)
                if failure is not None:
                    return format_response(failure, "ue_command_error", command=command)
                if command == "list_animation_bindings" and not detail:
                    # A summary read is a native-owned page: its continuation was computed for the
                    # whole page, so it is complete-or-refuse rather than shortened here.
                    return format_native_page(response.get("data", {}), qualified, route_params)
                return format_response(
                    response.get("data", {}),
                    f"{domain}_cmd",
                    command=command,
                    detail=detail,
                )

            # Native-owned pagination: the native window/selector is authoritative. A Python
            # cursor is unsupported for these reads and refuses before dispatch, and an oversized
            # native page is complete-or-refuse instead of being shortened.
            if (domain, command) in NATIVE_LIMIT_COMMANDS:
                if "cursor" in route_params:
                    return _invalid_cursor(
                        f"cursor is unsupported for native-owned {qualified}; "
                        "pass native limit/offset instead."
                    )
                response = connection.send_command(qualified, route_params)
                failure = _native_failure_payload(response)
                if failure is not None:
                    return format_response(
                        failure,
                        "ue_command_error",
                        command=command,
                        qualified_command=qualified,
                        request_params=route_params,
                    )
                return format_native_page(response.get("data", {}), qualified, route_params)

            # Cache-owned pagination only: a cursor means the read's primary array is explicit, so
            # any other read refuses before dispatch instead of returning an unrelated page.
            cursor_token = route_params.get("cursor")

            if cursor_token is not None:
                return _handle_cursor_request(cursor_token, qualified)

            # Check for limit (first page)
            limit_param = route_params.get("limit")
            if limit_param is not None:
                limit, error = _validate_limit(limit_param)
                if error:
                    return error
                if qualified in CACHED_PAGE_ARRAYS:
                    return _handle_limit_request(qualified, route_params, limit, connection)
                # Not a Python page owner: keep the parameter. Dropping it here would silently
                # discard what the caller sent and hide an undeclared `limit` from the native
                # declared-contract validator that exists to refuse it.

            # No pagination — normal dispatch
            if (domain, command) in _CACHED_READ_COMMANDS:
                response = connection.send_command_cached(
                    qualified,
                    route_params,
                    ttl=_CACHED_READ_TTL,
                )
                return format_response(
                    response.get("data", {}),
                    f"{domain}_cmd",
                    qualified_command=qualified,
                    request_params=route_params,
                )

            dispatch_timeout = _dispatch_timeout(qualified, route_params)
            if dispatch_timeout is None:
                response = connection.send_command(qualified, route_params)
            else:
                response = connection.send_command(qualified, route_params, timeout=dispatch_timeout)
            return format_response(
                response.get("data", {}),
                f"{domain}_cmd",
                qualified_command=qualified,
                request_params=route_params,
            )
        except ConnectionError as exc:
            return f"Error: {exc}"
        except UECommandError as exc:
            return _format_ue_command_error(
                exc,
                command=command,
                qualified_command=qualified,
                request_params=route_params,
            )
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
    # Replay replies are budgeted as one encoded envelope. The SDK's default auto-detection wraps a
    # string-returning router into both text and structuredContent.result, which would count the
    # same payload twice against that budget, so Replay opts out and every other domain keeps the
    # existing behaviour.
    mcp.tool(
        name=f"{domain}_cmd",
        description=docstring,
        structured_output=False if domain == "replay" else None,
    )(registered)

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


_CAPABILITIES_DETAIL_VALUES = ("names", "full")


def _project_capability_names(document: dict) -> dict:
    """Names-only projection of an authoritative capabilities document.

    Domains, command names and every non-signature field are preserved; only the per-command
    ``params`` signature inventory is dropped. A NEW document is built, so the authoritative
    response — and the Saved cache it was read from — is never mutated in place.
    """
    if not isinstance(document, dict):
        return document

    domains = document.get("domains")
    if not isinstance(domains, dict):
        return document

    projected_domains: dict[str, Any] = {}
    for domain, body in domains.items():
        if not isinstance(body, dict) or not isinstance(body.get("commands"), list):
            projected_domains[domain] = body
            continue
        projected_body = {k: v for k, v in body.items() if k != "commands"}
        projected_body["commands"] = [
            {k: v for k, v in command.items() if k != "params"}
            if isinstance(command, dict)
            else command
            for command in body["commands"]
        ]
        projected_domains[domain] = projected_body

    projected = {k: v for k, v in document.items() if k != "domains"}
    projected["domains"] = projected_domains
    return projected


def _get_capabilities(connection, params: dict) -> str:
    """Agent-facing capability inventory: ``detail=names`` (default) or ``detail=full``.

    ``detail`` is Python-owned presentation only. It is validated and removed before the native
    call, and the native ``get_capabilities`` command never receives it. The projection runs after
    the authoritative response arrives, so a projected view is never written back to the Saved
    capabilities cache and a following ``detail=full`` read still carries the signatures.
    """
    detail = params.get("detail", "names")
    if detail not in _CAPABILITIES_DETAIL_VALUES:
        return json.dumps({
            "_error": "INVALID_FIELD",
            "_message": "detail must be 'names' (default) or 'full'.",
        })

    native_params = {k: v for k, v in params.items() if k != "detail"}
    response = connection.send_command_cached(
        "get_capabilities",
        native_params,
        ttl=_CACHED_READ_TTL,
    )
    failure = _native_failure_payload(response)
    if failure is not None:
        return format_response(failure, "ue_command_error", command="get_capabilities")

    data = response.get("data", {})
    if detail == "full":
        return format_response(
            data,
            "get_capabilities",
            qualified_command="get_capabilities",
            request_params=native_params,
        )
    return format_response(
        _project_capability_names(data),
        "get_capabilities",
        qualified_command="get_capabilities",
        request_params=native_params,
    )


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
