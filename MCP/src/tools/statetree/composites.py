"""Composite helpers for high-level StateTree creation and updates."""

from __future__ import annotations

import json
import logging
from typing import Any

from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

_VALID_MODES = {"create", "update"}
_SUPPORTED_COMPOSE_COMMANDS = {
    "add_state",
    "remove_state",
    "rename_state",
    "move_state",
    "set_state_properties",
    "add_transition",
    "remove_transition",
    "set_transition_properties",
}
_STATE_SELECTOR_KEYS = (("state_id", "state_path"), ("parent_state_id", "parent_state_path"), ("new_parent_state_id", "new_parent_state_path"))
_TRANSITION_SELECTOR_KEYS = (
    ("state_id", "state_path"),
    ("source_state_id", "source_state_path"),
    ("target_state_id", "target_state_path"),
)


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


def _validate_supported_operations(planned_operations: list[dict[str, Any]]) -> None:
    """Reject commands outside the declared compose mutation surface."""
    for operation in planned_operations:
        command = operation["command"]
        if command not in _SUPPORTED_COMPOSE_COMMANDS:
            raise ValueError(f"Unsupported StateTree compose command: {command}")


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

    for state in states or []:
        synthesized.append({"command": "add_state", "params": dict(state)})

    for transition in transitions or []:
        synthesized.append({"command": "add_transition", "params": dict(transition)})

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


def _selector_value(params: dict[str, Any], id_key: str, path_key: str) -> tuple[str | None, str | None]:
    """Read one state selector pair from params."""
    state_id = params.get(id_key)
    state_path = params.get(path_key)
    return (state_id if isinstance(state_id, str) and state_id else None, state_path if isinstance(state_path, str) and state_path else None)


def _resolve_state_selector(
    params: dict[str, Any],
    id_key: str,
    path_key: str,
    states_by_id: dict[str, dict[str, Any]],
    state_path_counts: dict[str, int],
) -> dict[str, Any] | None:
    """Resolve a state selector from preflight state data."""
    state_id, state_path = _selector_value(params, id_key, path_key)
    if state_id and state_path:
        raise ValueError(f"Specify exactly one of {id_key} or {path_key}")
    if state_id:
        state = states_by_id.get(state_id)
        if state is None:
            raise ValueError(f"State not found: {state_id}")
        return state
    if state_path:
        count = state_path_counts.get(state_path, 0)
        if count == 0:
            raise ValueError(f"State path not found: {state_path}")
        if count > 1:
            raise ValueError(f"State path is ambiguous: {state_path}")
        for state in states_by_id.values():
            if state.get("path") == state_path:
                return state
    return None


def _build_preflight_state_maps(states: list[dict[str, Any]]) -> tuple[dict[str, dict[str, Any]], dict[str, int]]:
    """Build lookup maps for state selectors."""
    states_by_id: dict[str, dict[str, Any]] = {}
    state_path_counts: dict[str, int] = {}
    for state in states:
        state_id = state.get("id")
        state_path = state.get("path")
        if isinstance(state_id, str) and state_id:
            states_by_id[state_id] = state
        if isinstance(state_path, str) and state_path:
            state_path_counts[state_path] = state_path_counts.get(state_path, 0) + 1
    return states_by_id, state_path_counts


def _validate_operation_preflight(
    operation: dict[str, Any],
    states_by_id: dict[str, dict[str, Any]],
    state_path_counts: dict[str, int],
) -> None:
    """Validate selector references for one planned update operation."""
    command = operation["command"]
    params = operation["params"]

    if command in {"remove_state", "rename_state", "move_state", "set_state_properties", "remove_transition", "set_transition_properties"}:
        _resolve_state_selector(params, "state_id", "state_path", states_by_id, state_path_counts)

    if command == "add_state":
        parent = _resolve_state_selector(params, "parent_state_id", "parent_state_path", states_by_id, state_path_counts)
        if params.get("parent_state_id") or params.get("parent_state_path"):
            if parent is None:
                raise ValueError("Parent state not found")

    if command == "move_state":
        if params.get("new_parent_state_id") or params.get("new_parent_state_path"):
            _resolve_state_selector(params, "new_parent_state_id", "new_parent_state_path", states_by_id, state_path_counts)

    if command == "add_transition":
        _resolve_state_selector(params, "source_state_id", "source_state_path", states_by_id, state_path_counts)
        if params.get("target_state_id") or params.get("target_state_path"):
            _resolve_state_selector(params, "target_state_id", "target_state_path", states_by_id, state_path_counts)


def _apply_operation_to_preflight_state_maps(
    operation: dict[str, Any],
    states_by_id: dict[str, dict[str, Any]],
    state_path_counts: dict[str, int],
) -> None:
    """Apply a coarse local state-map update so later ops preflight against intended order."""
    command = operation["command"]
    params = operation["params"]

    if command == "remove_state":
        state = _resolve_state_selector(params, "state_id", "state_path", states_by_id, state_path_counts)
        if state is None:
            return
        state_id = state.get("id")
        state_path = state.get("path")
        if isinstance(state_id, str):
            states_by_id.pop(state_id, None)
        if isinstance(state_path, str) and state_path in state_path_counts:
            state_path_counts[state_path] = max(0, state_path_counts[state_path] - 1)
            if state_path_counts[state_path] == 0:
                state_path_counts.pop(state_path, None)
        return

    if command == "add_state":
        parent = _resolve_state_selector(params, "parent_state_id", "parent_state_path", states_by_id, state_path_counts)
        parent_path = parent.get("path") if parent else "Root"
        state_name = params.get("name")
        if isinstance(parent_path, str) and isinstance(state_name, str) and state_name:
            new_path = f"{parent_path}/{state_name}"
            state_path_counts[new_path] = state_path_counts.get(new_path, 0) + 1


def _preflight_update(
    connection: UEConnection,
    asset_path: str,
    expected_fingerprint: dict[str, Any],
    planned_operations: list[dict[str, Any]],
) -> dict[str, Any]:
    """Inspect update targets and reject invalid plans before any mutation runs."""
    dump_response = _send_statetree_command(
        connection,
        "dump_tree",
        {"asset_path": asset_path, "include_transitions": True, "include_nodes": False},
    )
    dump_data = _extract_data(dump_response)
    current_fingerprint = dump_data.get("fingerprint")
    if current_fingerprint != expected_fingerprint:
        raise ValueError("Expected fingerprint does not match current StateTree fingerprint")

    states = dump_data.get("states")
    if not isinstance(states, list):
        raise ValueError("StateTree preflight expected dump_tree to return states")

    states_by_id, state_path_counts = _build_preflight_state_maps([state for state in states if isinstance(state, dict)])
    for operation in planned_operations:
        _validate_operation_preflight(operation, states_by_id, state_path_counts)
        _apply_operation_to_preflight_state_maps(operation, states_by_id, state_path_counts)

    return dump_data


def register_statetree_composite_tools(mcp, connection: UEConnection) -> None:
    """Register high-level StateTree composite helpers."""

    @mcp.tool()
    def create_statetree_tree(
        asset_path: str,
        mode: str = "create",
        schema_class: str = "",
        root_name: str = "",
        validate: bool = True,
        compile: bool = True,
        save: bool = True,
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
            validate: Whether to run statetree.validate_asset after mutations.
            compile: Whether to run statetree.compile after mutations.
            save: Whether the final mutating step should persist the asset.
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
            _validate_supported_operations(planned_operations)
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
        total_steps = len(planned_operations) + (1 if mode == "create" else 0) + (1 if validate else 0) + (1 if compile else 0)
        created_asset = False
        validation_data: dict[str, Any] = {}
        compile_data: dict[str, Any] = {}

        try:
            if mode == "update":
                preflight_data = _preflight_update(connection, asset_path, dict(expected_fingerprint or {}), planned_operations)
                current_fingerprint = preflight_data.get("fingerprint", current_fingerprint)

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

            for operation_index, operation in enumerate(planned_operations):
                params = dict(operation["params"])
                params["asset_path"] = asset_path
                if current_fingerprint is not None and "expected_fingerprint" not in params:
                    params["expected_fingerprint"] = current_fingerprint
                params.setdefault("compile", False)
                params.setdefault(
                    "save",
                    bool(save and not validate and not compile and operation_index == len(planned_operations) - 1),
                )

                op_response = _send_statetree_command(connection, operation["command"], params)
                current_fingerprint = _extract_data(op_response).get("fingerprint", current_fingerprint)
                completed_steps += 1

            if validate:
                validate_params: dict[str, Any] = {"asset_path": asset_path, "save": bool(save and not compile)}
                if current_fingerprint is not None:
                    validate_params["expected_fingerprint"] = current_fingerprint
                validate_response = _send_statetree_command(connection, "validate_asset", validate_params)
                validation_data = _extract_data(validate_response)
                current_fingerprint = validation_data.get("fingerprint", current_fingerprint)
                completed_steps += 1

            if compile:
                compile_params: dict[str, Any] = {"asset_path": asset_path, "save": save}
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
        except ValueError as exc:
            return json.dumps(
                {
                    "success": False,
                    "mode": mode,
                    "asset_path": asset_path,
                    "error": str(exc),
                    "completed_steps": completed_steps,
                    "total_steps": total_steps,
                    "fingerprint": current_fingerprint,
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
