"""Shared pre-dispatch boundary for graph.apply_patch pagination and bounded approvals."""

from __future__ import annotations

import json
import uuid

from .response import MAX_RESPONSE_CHARS, format_response
from .tcp_client import UECommandError, UECommandNotDispatchedError

_GRAPH_PATCH_COMMAND = "graph.apply_patch"
_OUTCOME_FIELDS = (
    "patch_id",
    "changed",
    "dry_run",
    "apply_status",
    "compile_status",
    "readback_status",
    "rollback_status",
    "save_status",
    "post_save_status",
    "target_compile_count",
    "recovery_compile_count",
    "saved",
    "blocked",
    "replayed_with_absent_source",
    "fingerprint_before",
    "fingerprint_after",
    "dirty_before",
    "dirty_after",
    "locators",
)


_COMPLETE_APPROVAL_MIGRATIONS = {"prune_island", "retire_entries"}


def complete_approval_migration_op(request: dict[str, Any]) -> str | None:
    migration = request.get("migration")
    if not isinstance(migration, dict):
        return None
    op = migration.get("op")
    return op if op in _COMPLETE_APPROVAL_MIGRATIONS else None


def is_prune_patch(request: dict[str, Any]) -> bool:
    """Retained for the existing prune contract test; dispatch uses the generic predicate."""
    return complete_approval_migration_op(request) == "prune_island"

def reject_apply_patch_pagination(request: dict[str, Any]) -> str | None:
    fields = [key for key in ("limit", "cursor", "offset", "page") if key in request]
    if fields:
        return (
            "Pagination parameters (" + ", ".join(fields)
            + ") are not supported on graph.apply_patch."
        )
    return None


def _json_size(value: Any) -> int:
    return len(json.dumps(value, indent=2))


def _encode(value: Any) -> str:
    return json.dumps(value, indent=2)


def _identity(request: dict[str, Any]) -> dict[str, Any]:
    return {key: request[key] for key in ("patch_id", "asset_path") if key in request}


def _refusal(
    request: dict[str, Any],
    *,
    response_size: int | None = None,
    prospective_size: int | None = None,
    native_complete: bool | None = None,
    removable_count: int = 0,
    approved_count: int = 0,
    message: str = "The complete approval response exceeds the MCP response budget.",
) -> str:
    payload: dict[str, Any] = {
        "success": False,
        "_error": "LIMIT_EXCEEDED",
        "_message": message,
        **_identity(request),
        "approval_complete": False,
        "max_response_chars": MAX_RESPONSE_CHARS,
        "removable_count": removable_count,
        "approved_count": approved_count,
        **({"complete": native_complete} if native_complete is not None else {}),
    }
    if response_size is not None:
        payload["response_size_chars"] = response_size
    if prospective_size is not None:
        payload["prospective_apply_size_chars"] = prospective_size
    text = _encode(payload)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text
    payload["_message"] = "Approval response exceeds the MCP response budget."
    text = _encode(payload)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text
    for key in ("patch_id", "asset_path"):
        value = payload.get(key)
        if isinstance(value, str) and len(value) > 512:
            payload[key] = value[:512]
            payload[f"_{key}_truncated"] = True
    text = _encode(payload)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text
    payload.pop("asset_path", None)
    return _encode(payload)


def _prospective_apply_size(
    preview: dict[str, Any],
    request: dict[str, Any],
    approved_guids: list[Any],
) -> int:
    candidate = dict(preview)
    for field in _OUTCOME_FIELDS:
        candidate.setdefault(field, None)
    if "removable" in candidate:
        candidate["removable"] = preview["removable"]
    candidate["approved_guids"] = approved_guids
    candidate.update(_identity(request))
    candidate["success"] = True
    candidate["approval_complete"] = True
    return _json_size(candidate)


def _complete_approval_preview(
    data: dict[str, Any],
    request: dict[str, Any],
    operation: str,
) -> str:
    size = _json_size(data)
    removable = data.get("removable")
    approved = data.get("approved_guids")
    removable_count = len(removable) if isinstance(removable, list) else 0
    approved_count = len(approved) if isinstance(approved, list) else 0
    if data.get("complete") is True and size <= MAX_RESPONSE_CHARS:
        return _encode(data)
    return _refusal(
        request,
        response_size=size,
        removable_count=removable_count,
        approved_count=approved_count,
        native_complete=data.get("complete") if isinstance(data.get("complete"), bool) else None,
        message=(
            f"The {operation} scan is incomplete; approval requires a complete native preview."
            if data.get("complete") is not True
            else f"The complete {operation} preview exceeds the MCP response budget."
        ),
    )


def _bounded_success(data: dict[str, Any], request: dict[str, Any]) -> str:
    complete = _encode(data)
    if len(complete) <= MAX_RESPONSE_CHARS:
        return complete

    compact: dict[str, Any] = {key: data[key] for key in _OUTCOME_FIELDS if key in data}
    compact.update(_identity(request))
    omitted = [key for key in data if key not in compact]
    compact.update({
        "_truncated": True,
        "_omitted_fields": omitted,
        "original_response_size_chars": len(complete),
        "reconciliation_required": True,
        "_reconciliation_guidance": "Read back the patch state before treating the response as complete or retrying.",
    })
    # Authoritative outcome fields have priority; compact variable-size locators only if needed.
    if len(_encode(compact)) > MAX_RESPONSE_CHARS and isinstance(compact.get("locators"), list):
        omitted_locators = len(compact["locators"])
        compact["locators"] = []
        compact["_locators_omitted"] = omitted_locators
        compact["_omitted_fields"] = sorted(set(compact["_omitted_fields"] + ["locators"]))
    if len(_encode(compact)) > MAX_RESPONSE_CHARS:
        for key in ("fingerprint_before", "fingerprint_after"):
            if isinstance(compact.get(key), str) and len(compact[key]) > 1024:
                compact[key] = compact[key][:1024]
                compact["_outcome_values_truncated"] = True
                if key not in compact["_omitted_fields"]:
                    compact["_omitted_fields"].append(key)
    return _fit_payload(compact)


def _bounded_error(exc: UECommandError, request: dict[str, Any]) -> str:
    payload: dict[str, Any] = {
        "success": False,
        "_error": exc.code,
        "_message": exc.message,
        "_command": exc.command,
        **_identity(request),
    }
    payload.update({
        key: value
        for key, value in exc.details.items()
        if key not in {"success", "_error", "_message", "_command"}
    })
    full_size = _json_size(payload)
    if full_size <= MAX_RESPONSE_CHARS:
        return _encode(payload)

    # Drop optional bulky collections before shortening the native message.
    omitted_fields = []
    for key in ("diagnostics", "inventory", "removable", "approved_guids", "locators"):
        value = payload.get(key)
        if isinstance(value, (list, dict)):
            payload.pop(key)
            payload[f"_{key}_omitted"] = len(value)
            omitted_fields.append(key)
    if omitted_fields:
        payload["_omitted_fields"] = omitted_fields
    payload["_truncated"] = True
    payload["original_response_size_chars"] = full_size
    payload["reconciliation_required"] = True
    payload["_reconciliation_guidance"] = "Read back patch state before any retry; native execution may have changed the asset."
    return _fit_payload(payload)


def _record_omission(payload: dict[str, Any], key: str, value: Any) -> None:
    fields = payload.setdefault("_omitted_fields", [])
    label = key[:128]
    if label not in fields:
        fields.append(label)
    if len(key) <= 120:
        payload[f"_{key}_omitted"] = len(value) if isinstance(value, (list, dict)) else True
    else:
        payload["_other_field_omitted"] = True
    payload["_omitted_data"] = True
    payload["_truncated"] = True
    payload["reconciliation_required"] = True
    payload.setdefault(
        "_reconciliation_guidance",
        "Read back patch and asset state before any retry; the response omitted native outcome details.",
    )


def _fit_payload(payload: dict[str, Any]) -> str:
    text = _encode(payload)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text

    original_size = len(text)
    payload["_truncated"] = True
    payload.setdefault("original_response_size_chars", original_size)
    payload["reconciliation_required"] = True
    payload["_omitted_data"] = True
    payload.setdefault(
        "_reconciliation_guidance",
        "Read back patch and asset state before any retry; the response omitted native outcome details.",
    )

    for key in ("diagnostics", "inventory", "removable", "approved_guids", "locators"):
        if key in payload:
            _record_omission(payload, key, payload.pop(key))
    for key in ("fingerprint_before", "fingerprint_after"):
        value = payload.get(key)
        if isinstance(value, (dict, list)) or (isinstance(value, str) and len(value) > 4096):
            _record_omission(payload, key, payload.pop(key))

    for key, value in tuple(payload.items()):
        if isinstance(value, (dict, list)) and key != "_omitted_fields":
            _record_omission(payload, key, payload.pop(key))
    text = _encode(payload)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text

    # The final envelope keeps only native status/error identity and bounded reconciliation metadata.
    keep = {
        "success", "_error", "_command", "patch_id", "asset_path", "_message",
        "changed", "dry_run", "apply_status", "compile_status", "readback_status",
        "rollback_status", "save_status", "post_save_status", "target_compile_count",
        "recovery_compile_count", "saved", "blocked", "replayed_with_absent_source",
        "dirty_before", "dirty_after", "_truncated", "_message_truncated",
        "original_message_chars", "original_response_size_chars", "reconciliation_required",
        "_reconciliation_guidance", "_omitted_data", "_omitted_fields",
        "_diagnostics_omitted", "_inventory_omitted", "_removable_omitted",
        "_approved_guids_omitted", "_locators_omitted", "_other_field_omitted",
        "_fingerprint_before_omitted", "_fingerprint_after_omitted",
    }
    bounded = {key: value for key, value in payload.items() if key in keep}
    for key in tuple(bounded):
        value = bounded[key]
        if isinstance(value, (dict, list)) and key != "_omitted_fields":
            _record_omission(bounded, key, value)
            bounded.pop(key, None)
        elif isinstance(value, str) and len(value) > 512:
            bounded[key] = value[:512]
            if key == "_message":
                bounded["_message_truncated"] = True
                bounded["original_message_chars"] = len(value)
            elif key in ("patch_id", "asset_path"):
                bounded[f"_{key}_truncated"] = True
            elif key in ("_command", "_error", "_reconciliation_guidance"):
                bounded[key] = value[:512]
    if isinstance(bounded.get("_omitted_fields"), list):
        fields = bounded["_omitted_fields"]
        if len(fields) > 50:
            bounded["_omitted_fields"] = [str(field)[:128] for field in fields[:50]]
            bounded["_omitted_fields_count"] = len(fields)
        else:
            bounded["_omitted_fields"] = [str(field)[:128] for field in fields]
    return _encode(bounded)


def _canonical_guid_array(values: Any) -> tuple[str, ...] | None:
    if not isinstance(values, list):
        return None
    try:
        canonical = [uuid.UUID(value).hex for value in values if isinstance(value, str)]
    except (ValueError, AttributeError):
        return None
    if len(canonical) != len(values) or len(set(canonical)) != len(canonical):
        return None
    return tuple(sorted(canonical))


def _stale_precondition(request: dict[str, Any], message: str) -> str:
    return _fit_payload({
        "success": False,
        "_error": "STALE_PRECONDITION",
        "_message": message,
        "_command": _GRAPH_PATCH_COMMAND,
        **_identity(request),
    })


def _unknown_outcome(exc: ConnectionError, request: dict[str, Any]) -> str:
    return _fit_payload({
        "success": False,
        "_error": "UNKNOWN_OUTCOME",
        "_message": (
            "The apply request was dispatched but its outcome is unknown. "
            "Reconcile by reading back patch and asset state before any retry. "
            + str(exc)
        ),
        "_command": _GRAPH_PATCH_COMMAND,
        **_identity(request),
        "reconciliation_required": True,
        "_reconciliation_guidance": (
            "Read back patch and asset state before any retry; native execution may have changed the asset."
        ),
    })


def _approval_connection_error(exc: ConnectionError, request: dict[str, Any]) -> str:
    return _fit_payload({
        "success": False,
        "_error": "CONNECTION_ERROR",
        "_message": str(exc),
        "_command": _GRAPH_PATCH_COMMAND,
        **_identity(request),
    })

def _native_error(exc: UECommandError, request: dict[str, Any]) -> str:
    return _bounded_error(exc, request)

def _response_error(response: dict[str, Any], request: dict[str, Any]) -> str | None:
    if response.get("success") is not False:
        return None
    error = response.get("error", {})
    return _native_error(
        UECommandError(
            _GRAPH_PATCH_COMMAND,
            error.get("code", "UNKNOWN"),
            error.get("message", "Unknown error"),
            error.get("details", {}),
        ),
        request,
    )


def dispatch_graph_apply_patch(connection, request: dict[str, Any], *, tool_name: str) -> str:
    pagination_error = reject_apply_patch_pagination(request)
    if pagination_error:
        return _encode({"success": False, "_error": "INVALID_FIELD", "_message": pagination_error})
    operation = complete_approval_migration_op(request)
    if operation is None:
        try:
            response = connection.send_command(_GRAPH_PATCH_COMMAND, request)
            error = _response_error(response, request)
            if error is not None:
                return error
            return format_response(response.get("data", {}), tool_name)
        except UECommandError as exc:
            return _native_error(exc, request)
        except ConnectionError as exc:
            return _encode({"success": False, "_error": "CONNECTION_ERROR", "_message": str(exc)})

    applying = request.get("dry_run") is False
    if "dry_run" in request and not isinstance(request["dry_run"], bool):
        try:
            response = connection.send_command(_GRAPH_PATCH_COMMAND, request)
            error = _response_error(response, request)
            if error is not None:
                return error
            return _complete_approval_preview(response.get("data", {}), request, operation)
        except UECommandError as exc:
            return _native_error(exc, request)
        except ConnectionError as exc:
            return _approval_connection_error(exc, request)

    if not applying:
        try:
            response = connection.send_command(_GRAPH_PATCH_COMMAND, request)
            error = _response_error(response, request)
            if error is not None:
                return error
            return _complete_approval_preview(response.get("data", {}), request, operation)
        except UECommandError as exc:
            return _native_error(exc, request)
        except ConnectionError as exc:
            return _approval_connection_error(exc, request)

    migration = request["migration"]
    caller_token = request.get("expected_validation_hash")
    if not isinstance(caller_token, str) or not caller_token.strip():
        return _stale_precondition(
            request,
            f"A {operation} apply requires the nonempty validation hash returned by the caller's approved preview.",
        )

    preview_request = dict(request)
    preview_request.pop("expected_validation_hash", None)
    preview_request["dry_run"] = True
    preview_request["save"] = False
    try:
        response = connection.send_command(_GRAPH_PATCH_COMMAND, preview_request)
    except UECommandError as exc:
        return _native_error(exc, request)
    except ConnectionError as exc:
        return _approval_connection_error(exc, request)
    error = _response_error(response, request)
    if error is not None:
        return error
    preview = response.get("data", {})
    preview_text = _complete_approval_preview(preview, request, operation)
    if _json_size(preview) > MAX_RESPONSE_CHARS or preview.get("complete") is not True:
        return preview_text

    if preview.get("validation_hash") != caller_token:
        return _stale_precondition(
            request,
            f"The {operation} validation hash changed after the caller's preview; review and approve a fresh preview.",
        )

    requested_approved = migration.get("approved_node_guids")
    caller_approved = _canonical_guid_array(requested_approved)
    native_approved = _canonical_guid_array(preview.get("approved_guids"))
    if (
        caller_approved is None
        or not caller_approved
        or native_approved is None
        or native_approved != caller_approved
    ):
        return _stale_precondition(
            request,
            "The native approved GUID set differs from the nonempty GUID set explicitly approved by the caller.",
        )

    approved = requested_approved
    prospective_size = _prospective_apply_size(preview, request, approved)
    removable = preview.get("removable")
    removable_count = len(removable) if isinstance(removable, list) else 0
    if prospective_size > MAX_RESPONSE_CHARS:
        return _refusal(
            request,
            prospective_size=prospective_size,
            native_complete=preview.get("complete") is True,
            removable_count=removable_count,
            approved_count=len(approved),
            message=f"The conservative prospective {operation} apply response exceeds the MCP response budget.",
        )

    try:
        result = connection.send_command_once(_GRAPH_PATCH_COMMAND, request)
    except UECommandNotDispatchedError as exc:
        return _approval_connection_error(exc, request)
    except UECommandError as exc:
        return _native_error(exc, request)
    except ConnectionError as exc:
        return _unknown_outcome(exc, request)
    error = _response_error(result, request)
    if error is not None:
        return error
    return _bounded_success(result.get("data", {}), request)
