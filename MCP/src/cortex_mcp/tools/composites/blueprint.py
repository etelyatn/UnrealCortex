"""Explicit registration for blueprint composite tools."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Optional

_MCP_ROOT = Path(__file__).resolve().parents[4]
if str(_MCP_ROOT) not in sys.path:
    sys.path.insert(0, str(_MCP_ROOT))

from cortex_mcp.graph_patch_boundary import dispatch_graph_apply_patch
from tools.blueprint.composites import register_blueprint_composite_tools

_BP_DISAMBIG = (
    "COMPOSITE tool — use for creating or updating a Blueprint with variables, "
    "functions, and graph logic. For individual Blueprint commands use blueprint_cmd.\n\n"
)

_SAFE_UPDATE_DOC = (
    "\n\nSafe update mode:\n"
    "  blueprint_compose(mode='update', asset_path='/Game/BP_X.BP_X', patch={...})\n"
    "A non-prune update sends the reviewed patch once in a `graph.apply_patch` call. A prune\n"
    "preview must be complete and fit the response budget. A prune apply requires the validation\n"
    "hash from a separate preview of the exact approved GUID set, then uses a bounded preflight\n"
    "followed by one one-shot apply; the initial partition preview hash is not sufficient. If the\n"
    "apply result is ambiguous, perform readback reconciliation before any retry. The facade adds\n"
    "asset_path and sends `patch` unchanged, so native validation stays authoritative:\n"
    "  {'patch_id': <uuid>, 'target': {'graph_ref'|'implementation': ...},\n"
    "   'expected_fingerprint': <graph.get_authoring_context fingerprint>, 'nodes': [],\n"
    "   'connections': [], 'pin_updates': [], 'dry_run': true, 'compile': true, 'save': false,\n"
    "   'allow_noop': false, 'expected_validation_hash': <from a matching preview>}\n"
    "Legacy update fields (nodes, connections, variables, functions, expected_fingerprint, ...) are\n"
    "refused in update mode; the legacy batch update route was removed and is never a fallback.\n"
)

# Declared defaults of the legacy create-mode fields. A caller that supplies one of these values
# alongside `patch` is mixing the two update contracts; the facade refuses instead of guessing.
_LEGACY_FIELD_DEFAULTS = {
    "name": "",
    "path": "",
    "type": "Actor",
    "parent_class": "",
    "variables": None,
    "functions": None,
    "graph_name": "EventGraph",
    "nodes": None,
    "connections": None,
    "subgraph_path": "",
    "graph_kind": "",
    "owning_interface": "",
    "expected_fingerprint": None,
}

# Flag fields whose type is strict in the native envelope; the facade enforces shape only.
_PATCH_FLAGS = ("dry_run", "compile", "save", "allow_noop")


def _local_error(code: str, message: str) -> str:
    return json.dumps({"success": False, "_error": code, "_message": message}, indent=2)


def _is_legacy_supplied(field: str, value: object) -> bool:
    """A declared default (or an empty collection) is not a supplied legacy field."""
    if value in (None, "", [], {}):
        return False
    return value != _LEGACY_FIELD_DEFAULTS[field]


def _safe_update(
    connection,
    asset_path: str,
    patch: Optional[dict],
    legacy_supplied: list[str],
) -> str:
    """Dispatch a reviewed patch without falling back to the legacy batch route."""
    if patch is None:
        legacy_note = (
            f" The legacy fields supplied with this call ({', '.join(legacy_supplied)}) are no "
            "longer accepted."
            if legacy_supplied
            else ""
        )
        return _local_error(
            "MIGRATION_REQUIRED",
            "mode='update' requires a 'patch' object: send the reviewed envelope to "
            "blueprint_compose(mode='update', asset_path=<asset>, patch={...}), which uses the "
            "graph.apply_patch contract: non-prune updates send once, while prune applies use a "
            "bounded preflight and one one-shot apply. The legacy batch update route was removed "
            "and is never used as a fallback."
            f"{legacy_note}",
        )
    if legacy_supplied:
        return _local_error(
            "MIXED_UPDATE_CONTRACT",
            f"mode='update' cannot mix legacy fields ({', '.join(legacy_supplied)}) with 'patch'; "
            "move the request into the patch envelope (the stale-write guard belongs in "
            "patch.expected_fingerprint) and resend.",
        )
    if not isinstance(patch, dict):
        return _local_error(
            "INVALID_PATCH", f"'patch' must be an object, got {type(patch).__name__}",
        )
    if not patch:
        return _local_error(
            "INVALID_PATCH",
            "'patch' must not be empty; supply the graph.apply_patch envelope to forward",
        )
    if not asset_path:
        return _local_error(
            "INVALID_PATCH", "Missing required field: asset_path (required in update mode)",
        )
    # The facade owns the envelope asset: any asset_path key inside the patch (including an
    # explicit null) is duplicate ownership, so it is refused before forwarding.
    if "asset_path" in patch and patch["asset_path"] != asset_path:
        return _local_error(
            "INVALID_PATCH",
            f"patch.asset_path '{patch['asset_path']}' conflicts with the envelope asset_path "
            f"'{asset_path}': the facade owns the envelope asset, so omit patch.asset_path or "
            "pass the same asset",
        )
    for flag in _PATCH_FLAGS:
        if flag in patch and not isinstance(patch[flag], bool):
            return _local_error(
                "INVALID_PATCH",
                f"patch.{flag} must be a boolean, got {type(patch[flag]).__name__}; "
                "flag types are never coerced",
            )

    request = {"asset_path": asset_path, **patch}
    return dispatch_graph_apply_patch(connection, request, tool_name="blueprint_compose")


def register_blueprint_compose_tools(mcp, connection) -> None:
    """Register blueprint composition tools."""
    # _CaptureMCP intercepts legacy tool registration so we can re-export
    # under consolidated names without reimplementing composite logic.
    # Tech debt: remove once composites are migrated to self-contained modules.
    captured: dict[str, callable] = {}

    class _CaptureMCP:
        def tool(self, name=None, description=None, **_kwargs):
            def decorator(fn):
                captured[name or fn.__name__] = fn
                return fn

            return decorator

    register_blueprint_composite_tools(_CaptureMCP(), connection)

    # Build description BEFORE decoration — FastMCP reads description= at decoration time
    _bp_doc = _BP_DISAMBIG + _SAFE_UPDATE_DOC + (captured.get("create_blueprint_graph").__doc__ or "")

    @mcp.tool(name="blueprint_compose", description=_bp_doc)
    def blueprint_compose(
        name: str = "",
        path: str = "",
        type: str = "Actor",
        parent_class: str = "",
        variables: Optional[list[dict]] = None,
        functions: Optional[list[dict]] = None,
        graph_name: str = "EventGraph",
        nodes: Optional[list[dict]] = None,
        connections: Optional[list[dict]] = None,
        mode: str = "create",
        asset_path: str = "",
        subgraph_path: str = "",
        graph_kind: str = "",
        owning_interface: str = "",
        expected_fingerprint: Optional[dict] = None,
        patch: Optional[dict] = None,
    ) -> str:
        if mode == "update":
            legacy_supplied = [
                field
                for field, value in (
                    ("name", name),
                    ("path", path),
                    ("type", type),
                    ("parent_class", parent_class),
                    ("variables", variables),
                    ("functions", functions),
                    ("graph_name", graph_name),
                    ("nodes", nodes),
                    ("connections", connections),
                    ("subgraph_path", subgraph_path),
                    ("graph_kind", graph_kind),
                    ("owning_interface", owning_interface),
                    ("expected_fingerprint", expected_fingerprint),
                )
                if _is_legacy_supplied(field, value)
            ]
            return _safe_update(connection, asset_path, patch, legacy_supplied)
        if mode == "create":
            if patch is not None:
                return _local_error(
                    "INVALID_PATCH",
                    "'patch' is only valid with mode='update'; create mode takes the Blueprint "
                    "spec fields",
                )
            return captured["create_blueprint_graph"](
                name=name,
                path=path,
                type=type,
                parent_class=parent_class,
                variables=variables,
                functions=functions,
                graph_name=graph_name,
                nodes=nodes,
                connections=connections,
                mode=mode,
                asset_path=asset_path,
                subgraph_path=subgraph_path,
                graph_kind=graph_kind,
                owning_interface=owning_interface,
                expected_fingerprint=expected_fingerprint,
            )
        return _local_error(
            "UNSUPPORTED_MODE",
            f"Unsupported mode '{mode}': use mode='create' to create a Blueprint or "
            "mode='update' with a 'patch' object",
        )
