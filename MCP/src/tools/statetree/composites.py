"""Composite helpers for high-level StateTree creation and updates."""

from __future__ import annotations

import json
import logging
from typing import Any

from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

_VALID_MODES = {"create", "update"}


class _ComposeFailure(RuntimeError):
    """Internal failure wrapper with command context."""

    def __init__(self, command: str, message: str, response: dict[str, Any] | None = None):
        super().__init__(message)
        self.command = command
        self.response = response or {}


def _coerce_response(response: Any) -> dict[str, Any]:
    """Normalize TCP responses to dictionaries."""
    if isinstance(response, str):
        return json.loads(response)
    if isinstance(response, dict):
        return response
    raise TypeError(f"Unsupported response type: {type(response)!r}")


def _extract_data(response: dict[str, Any]) -> dict[str, Any]:
    """Return the payload dict from a command response."""
    data = response.get("data", response)
    return data if isinstance(data, dict) else {}


def _error_message(response: dict[str, Any], fallback: str) -> str:
    """Extract the most specific error message available."""
    return (
        response.get("error")
        or response.get("error_message")
        or _extract_data(response).get("error")
        or fallback
    )


def _normalize_operation(operation: dict[str, Any]) -> dict[str, Any]:
    """Normalize explicit operation specs to command/params form."""
    command = operation.get("command") or operation.get("op")
    if not command:
        raise ValueError("Operation missing 'command'")

    params = dict(operation.get("params") or {})
    if not params:
        params = {
            key: value
            for key, value in operation.items()
            if key not in {"command", "op", "params"}
        }

    if command.startswith("statetree."):
        command = command.split(".", 1)[1]

    return {"command": command, "params": params}


def _synthesize_operations(
    operations: list[dict[str, Any]] | None = None,
    states: list[dict[str, Any]] | None = None,
    transitions: list[dict[str, Any]] | None = None,
    removals: dict[str, list[dict[str, Any]]] | list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    """Combine explicit operations with convenience state/transition specs."""
    synthesized: list[dict[str, Any]] = []

    for operation in operations or []:
        synthesized.append(_normalize_operation(operation))

    for state in states or []:
        synthesized.append({"command": "add_state", "params": dict(state)})

    for transition in transitions or []:
        synthesized.append({"command": "add_transition", "params": dict(transition)})

    if isinstance(removals, dict):
        for state in removals.get("states", []):
            synthesized.append({"command": "remove_state", "params": dict(state)})
        for transition in removals.get("transitions", []):
            synthesized.append({"command": "remove_transition", "params": dict(transition)})
    elif isinstance(removals, list):
        for removal in removals:
            command = removal.get("command") or removal.get("op")
            if command:
                synthesized.append(_normalize_operation(removal))
                continue

            removal_type = removal.get("type")
            if removal_type == "state":
                params = {k: v for k, v in removal.items() if k != "type"}
                synthesized.append({"command": "remove_state", "params": params})
            elif removal_type == "transition":
                params = {k: v for k, v in removal.items() if k != "type"}
                synthesized.append({"command": "remove_transition", "params": params})
            else:
                raise ValueError("Removal entries must specify type 'state' or 'transition'")

    return synthesized


def _validate_compose_spec(
    *,
    mode: str,
    asset_path: str,
    schema_class: str,
    expected_fingerprint: dict[str, Any] | None,
) -> None:
    """Validate top-level compose arguments."""
    if mode not in _VALID_MODES:
        raise ValueError("mode must be 'create' or 'update'")
    if not asset_path:
        raise ValueError("Missing required field: asset_path")
    if mode == "create" and not schema_class:
        raise ValueError("Missing required field: schema_class (required in create mode)")
    if mode == "update" and expected_fingerprint is None:
        raise ValueError("Missing required field: expected_fingerprint (required in update mode)")


def _send_statetree_command(
    connection: UEConnection,
    command: str,
    params: dict[str, Any],
) -> dict[str, Any]:
    """Send a StateTree command and raise on failure."""
    response = _coerce_response(connection.send_command(f"statetree.{command}", params))
    if response.get("success", True) is False:
        raise _ComposeFailure(
            command=f"statetree.{command}",
            message=_error_message(response, f"statetree.{command} failed"),
            response=response,
        )
    return response


def _build_cleanup_params(asset_path: str, fingerprint: dict[str, Any] | None) -> dict[str, Any]:
    """Build delete parameters for best-effort create-mode cleanup."""
    params: dict[str, Any] = {"asset_path": asset_path, "force": True}
    if fingerprint is not None:
        params["expected_fingerprint"] = fingerprint
    return params


def register_statetree_composite_tools(mcp, connection: UEConnection) -> None:
    """Register high-level StateTree composite helpers."""

    @mcp.tool()
    def create_statetree_tree(
        asset_path: str,
        mode: str = "create",
        schema_class: str = "",
        root_name: str = "",
        operations: list[dict] | None = None,
        states: list[dict] | None = None,
        transitions: list[dict] | None = None,
        removals: dict | list[dict] | None = None,
        expected_fingerprint: dict | None = None,
    ) -> str:
        """Create or update a StateTree asset with sequential mutation steps.

        Args:
            asset_path: Target StateTree asset path.
            mode: Operation mode. Use 'create' for new assets or 'update' for
                existing assets. Only 'create' and 'update' are valid.
            schema_class: Required in create mode. StateTree schema class path
                or name passed to statetree.create_asset.
            root_name: Optional root state display name for create mode.
            operations: Optional explicit operation list. Each item may use
                {'command': 'add_state', 'params': {...}} or {'op': 'add_state', ...}.
            states: Optional shorthand list synthesized into statetree.add_state calls.
            transitions: Optional shorthand list synthesized into statetree.add_transition calls.
            removals: Optional shorthand for remove_state/remove_transition calls.
            expected_fingerprint: Required in update mode. Used as the initial
                stale-write guard and then refreshed after each successful step.

        Returns:
            JSON payload with success/failure metadata, step counts, and the
            latest known fingerprint. Create mode performs best-effort cleanup
            with statetree.delete_asset if any later step fails.
        """
        try:
            _validate_compose_spec(
                mode=mode,
                asset_path=asset_path,
                schema_class=schema_class,
                expected_fingerprint=expected_fingerprint,
            )
            planned_operations = _synthesize_operations(
                operations=operations,
                states=states,
                transitions=transitions,
                removals=removals,
            )
        except (TypeError, ValueError) as exc:
            return json.dumps(
                {
                    "success": False,
                    "mode": mode,
                    "asset_path": asset_path,
                    "error": str(exc),
                }
            )

        current_fingerprint = dict(expected_fingerprint) if expected_fingerprint is not None else None
        completed_steps = 0
        total_steps = len(planned_operations) + 2 + (1 if mode == "create" else 0)
        created_asset = False
        validation_data: dict[str, Any] = {}
        compile_data: dict[str, Any] = {}

        try:
            if mode == "create":
                create_params: dict[str, Any] = {
                    "asset_path": asset_path,
                    "schema_class": schema_class,
                    "save": False,
                }
                if root_name:
                    create_params["root_name"] = root_name

                create_response = _send_statetree_command(connection, "create_asset", create_params)
                create_data = _extract_data(create_response)
                asset_path = create_data.get("asset_path", asset_path)
                current_fingerprint = create_data.get("fingerprint", current_fingerprint)
                created_asset = True
                completed_steps += 1

            for operation in planned_operations:
                params = dict(operation["params"])
                params["asset_path"] = asset_path
                if current_fingerprint is not None and "expected_fingerprint" not in params:
                    params["expected_fingerprint"] = current_fingerprint

                op_response = _send_statetree_command(connection, operation["command"], params)
                current_fingerprint = _extract_data(op_response).get("fingerprint", current_fingerprint)
                completed_steps += 1

            validate_params: dict[str, Any] = {"asset_path": asset_path, "save": False}
            if current_fingerprint is not None:
                validate_params["expected_fingerprint"] = current_fingerprint
            validate_response = _send_statetree_command(connection, "validate_asset", validate_params)
            validation_data = _extract_data(validate_response)
            current_fingerprint = validation_data.get("fingerprint", current_fingerprint)
            completed_steps += 1

            compile_params: dict[str, Any] = {"asset_path": asset_path, "save": True}
            if current_fingerprint is not None:
                compile_params["expected_fingerprint"] = current_fingerprint
            compile_response = _send_statetree_command(connection, "compile", compile_params)
            compile_data = _extract_data(compile_response)
            current_fingerprint = compile_data.get("fingerprint", current_fingerprint)
            completed_steps += 1

            return json.dumps(
                {
                    "success": True,
                    "mode": mode,
                    "asset_path": asset_path,
                    "completed_steps": completed_steps,
                    "total_steps": total_steps,
                    "operations_executed": len(planned_operations),
                    "fingerprint": current_fingerprint,
                    "validation": validation_data,
                    "compile": compile_data,
                }
            )
        except _ComposeFailure as exc:
            cleanup: dict[str, Any] | None = None

            if mode == "create" and created_asset:
                cleanup_params = _build_cleanup_params(asset_path, current_fingerprint)
                try:
                    cleanup_response = _send_statetree_command(connection, "delete_asset", cleanup_params)
                    cleanup = {
                        "attempted": True,
                        "command": "statetree.delete_asset",
                        "success": True,
                        "params": cleanup_params,
                        "result": _extract_data(cleanup_response),
                    }
                except Exception as cleanup_exc:  # pragma: no cover - defensive path
                    logger.warning("StateTree cleanup failed for %s: %s", asset_path, cleanup_exc)
                    cleanup = {
                        "attempted": True,
                        "command": "statetree.delete_asset",
                        "success": False,
                        "params": cleanup_params,
                        "error": str(cleanup_exc),
                    }

            return json.dumps(
                {
                    "success": False,
                    "mode": mode,
                    "asset_path": asset_path,
                    "error": str(exc),
                    "summary": f"statetree_compose failed during {exc.command}",
                    "completed_steps": completed_steps,
                    "total_steps": total_steps,
                    "failed_step": {
                        "command": exc.command,
                        "error": str(exc),
                        "response": exc.response,
                    },
                    "fingerprint": current_fingerprint,
                    "cleanup": cleanup,
                }
            )
