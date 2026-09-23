"""Shared bounded dispatch boundary for graph.apply_patch prune migrations."""

from __future__ import annotations

import json
from typing import Any

from .response import MAX_RESPONSE_CHARS, format_response
from .tcp_client import UECommandError

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


def is_prune_patch(request: dict[str, Any]) -> bool:
    migration = request.get("migration")
    return isinstance(migration, dict) and migration.get("op") == "prune_island"


def reject_apply_patch_pagination(request: dict[str, Any]) -> str | None:
    if is_prune_patch(request):
        fields = [key for key in ("limit", "cursor", "offset", "page") if key in request]
        if fields:
            return (
                "Pagination parameters (" + ", ".join(fields)
                + ") are not supported on graph.apply_patch prune migrations."
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
    message: str = "The complete prune response exceeds the MCP response budget.",
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
    payload["_message"] = "Prune response exceeds the MCP response budget."
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


def _prune_preview(data: dict[str, Any], request: dict[str, Any]) -> str:
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
            "The prune scan is incomplete; approval requires a complete native preview."
            if data.get("complete") is not True
            else "The complete prune preview exceeds the MCP response budget."
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
    return _fit_payload(compact, fallback_kind="success")


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
    for key in ("diagnostics", "inventory", "removable", "approved_guids", "locators"):
        value = payload.get(key)
        if isinstance(value, (list, dict)):
            payload.pop(key)
            payload[f"_{key}_omitted"] = len(value)
    payload["_truncated"] = True
    payload["_message_truncated"] = True
    payload["original_message_chars"] = len(exc.message)
    payload["original_response_size_chars"] = full_size
    payload["reconciliation_required"] = True
    payload["_reconciliation_guidance"] = "Read back patch state before any retry; native execution may have changed the asset."
    return _fit_payload(payload, fallback_kind="error")


def _fit_payload(payload: dict[str, Any], *, fallback_kind: str) -> str:
    text = _encode(payload)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text

    # Preserve outcome/error identity and explicit evidence of omitted detail.
    payload["_omitted_data"] = True
    for key in ("diagnostics", "inventory", "removable", "approved_guids", "locators"):
        value = payload.pop(key, None)
        if value is not None:
            payload[f"_{key}_omitted"] = len(value) if isinstance(value, (list, dict)) else True
    message_key = "_message" if fallback_kind == "error" else "_reconciliation_guidance"
    if isinstance(payload.get(message_key), str):
        payload[message_key] = payload[message_key][:512]
    text = _encode(payload)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text

    # Last-resort bounded envelope retains native outcome values only as far as they fit.
    keep = {"success", "_error", "_command", "patch_id", "asset_path", "changed", "dry_run",
            "apply_status", "compile_status", "readback_status", "rollback_status", "save_status",
        "_message",
        "_message_truncated",
        "original_message_chars",
        "_diagnostics_omitted",
        "_inventory_omitted",
        "_removable_omitted",
        "_approved_guids_omitted",
        "_locators_omitted",
            "post_save_status", "target_compile_count", "recovery_compile_count", "saved", "blocked",
            "replayed_with_absent_source", "fingerprint_before", "fingerprint_after", "dirty_before",
            "dirty_after", "_truncated", "original_response_size_chars", "reconciliation_required",
            "_reconciliation_guidance", "_omitted_data"}
    bounded = {key: value for key, value in payload.items() if key in keep}
    for key in ("_message", "_reconciliation_guidance"):
        if key in bounded and isinstance(bounded[key], str):
            bounded[key] = bounded[key][:512]
    text = _encode(bounded)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text
    # Arbitrarily future-sized outcome fields are never allowed to exceed the transport budget.
    for key, value in tuple(bounded.items()):
        if isinstance(value, str) and len(value) > 512:
            bounded[key] = value[:512]
    return _encode(bounded)


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
    if not is_prune_patch(request):
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
            return _prune_preview(response.get("data", {}), request)
        except UECommandError as exc:
            return _native_error(exc, request)
        except ConnectionError as exc:
            return _encode({"success": False, "_error": "CONNECTION_ERROR", "_message": str(exc)})

    if not applying:
        try:
            response = connection.send_command(_GRAPH_PATCH_COMMAND, request)
            error = _response_error(response, request)
            if error is not None:
                return error
            return _prune_preview(response.get("data", {}), request)
        except UECommandError as exc:
            return _native_error(exc, request)
        except ConnectionError as exc:
            return _encode({"success": False, "_error": "CONNECTION_ERROR", "_message": str(exc)})

    migration = request["migration"]
    preview_request = dict(request)
    preview_request.pop("expected_validation_hash", None)
    preview_request["dry_run"] = True
    preview_request["save"] = False
    try:
        response = connection.send_command(_GRAPH_PATCH_COMMAND, preview_request)
    except UECommandError as exc:
        return _native_error(exc, request)
    except ConnectionError as exc:
        return _encode({"success": False, "_error": "CONNECTION_ERROR", "_message": str(exc)})
    error = _response_error(response, request)
    if error is not None:
        return error
    preview = response.get("data", {})
    preview_text = _prune_preview(preview, request)
    if _json_size(preview) > MAX_RESPONSE_CHARS or preview.get("complete") is not True:
        return preview_text

    caller_token = request.get("expected_validation_hash")
    first_token = preview.get("validation_hash")
    if caller_token is not None and first_token != caller_token:
        return _encode({
            "success": False,
            "_error": "STALE_PRECONDITION",
            "_message": "The prune validation hash changed after the caller's preview; review and approve a fresh preview.",
            **_identity(request),
        })

    requested_approved = migration.get("approved_node_guids", [])
    approved = list(requested_approved) if requested_approved else list(preview.get("approved_guids", []))
    second_request = dict(preview_request)
    second_migration = dict(migration)
    second_migration["approved_node_guids"] = approved
    second_request["migration"] = second_migration
    second_request["expected_validation_hash"] = first_token
    try:
        second_response = connection.send_command(_GRAPH_PATCH_COMMAND, second_request)
    except UECommandError as exc:
        return _native_error(exc, request)
    except ConnectionError as exc:
        return _encode({"success": False, "_error": "CONNECTION_ERROR", "_message": str(exc)})
    error = _response_error(second_response, request)
    if error is not None:
        return error
    second = second_response.get("data", {})
    if second.get("complete") is not True:
        return _prune_preview(second, request)
    if second.get("approved_guids", approved) != approved:
        return _encode({
            "success": False,
            "_error": "STALE_PRECONDITION",
            "_message": "The native approved GUID set differs from the requested prune set.",
            **_identity(request),
        })
    token = second.get("validation_hash")
    if not isinstance(token, str) or not token:
        return _encode({
            "success": False,
            "_error": "STALE_PRECONDITION",
            "_message": "The complete approval preview did not provide a validation hash.",
            **_identity(request),
        })

    prospective_size = _prospective_apply_size(second, request, approved)
    removable = second.get("removable")
    removable_count = len(removable) if isinstance(removable, list) else 0
    if prospective_size > MAX_RESPONSE_CHARS:
        return _refusal(
            request,
            prospective_size=prospective_size,
            native_complete=second.get("complete") is True,
            removable_count=removable_count,
            approved_count=len(approved),
            message="The conservative prospective prune apply response exceeds the MCP response budget.",
        )

    apply_request = dict(request)
    apply_request["expected_validation_hash"] = token
    try:
        result = connection.send_command_once(_GRAPH_PATCH_COMMAND, apply_request)
    except UECommandError as exc:
        return _native_error(exc, request)
    except ConnectionError as exc:
        return _encode({
            "success": False,
            "_error": "UNKNOWN_OUTCOME",
            "_message": "The apply request was dispatched but its outcome is unknown. Reconcile by reading back patch and asset state before any retry. " + str(exc),
            **_identity(request),
            "reconciliation_required": True,
        })
    error = _response_error(result, request)
    if error is not None:
        return error
    return _bounded_success(result.get("data", {}), request)
