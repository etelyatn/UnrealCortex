"""Explicit registration for blueprint composite tools."""

from __future__ import annotations

import json
from typing import Optional

from cortex_mcp.graph_patch_boundary import dispatch_graph_apply_patch
from tools.blueprint.composites import register_blueprint_composite_tools

_BP_DISAMBIG = (
    "COMPOSITE tool — use for creating or updating a Blueprint with variables, "
    "functions, and graph logic. For individual Blueprint commands use blueprint_cmd.\n"
)

_CREATE_DOC = (
    "\nmode='create' (default) creates a new Blueprint in one stop-on-error batch: variables,\n"
    "functions, graph nodes, pin values and connections. A partial asset is deleted when a step\n"
    "fails; this is recovery, not an atomic transaction. Fields: name, path, type (Actor,\n"
    "Component, Interface, FunctionLibrary — Widget Blueprints use widget_compose), parent_class\n"
    "(optional C++ class path, e.g. \"/Script/Engine.Character\"), graph_name (default\n"
    "\"EventGraph\"), variables ([{name, type, default_value?, is_exposed?, category?}]),\n"
    "functions ([{name}]), nodes, connections.\n"
    "\n"
    "nodes: [{name, class, params?, pin_values?, pin_text_values?}]. Common classes: Event,\n"
    "CustomEvent, CallFunction, Branch, Sequence, VariableGet, VariableSet, SpawnActor, CastTo,\n"
    "Timeline (params.timeline_name), MacroInstance (params.macro_path), AddDelegate/BindEvent\n"
    "(params.delegate_name + optional params.delegate_class; RemoveDelegate/UnbindEvent and\n"
    "ClearDelegate/UnbindAllEvents take the same params), CreateDelegate/CreateEvent\n"
    "(params.function_name — bare name, not Class.Function). ForEach loops are CallFunction with\n"
    "function_name \"KismetArrayLibrary.Array_ForEach\". pin_text_values is a map keyed by pin\n"
    "name: {\"InText\": {\"type\": \"FText\", \"source_kind\": \"string_table\", \"value\": \"Pay\",\n"
    "\"string_table\": {\"table_id\": \"/Game/UI/ST_UI.ST_UI\", \"key\": \"Mail.Button.Pay\"}}}.\n"
    "\n"
    "connections: [{from: \"NodeName.PinName\", to: \"NodeName.PinName\"}] (e.g. \"BeginPlay.then\" ->\n"
    "\"PrintString.execute\"). Common pin names: Event outputs \"then\"; CallFunction inputs\n"
    "\"execute\", outputs \"then\"; Branch \"Condition\"/\"True\"/\"False\"; Sequence \"then 0\",\n"
    "\"then 1\", ...; VariableGet output is the variable name; VariableSet inputs \"execute\" +\n"
    "variable name, outputs \"then\"; AddDelegate/RemoveDelegate inputs \"self\" (Target) and\n"
    "\"Delegate\" (Event); CreateDelegate inputs \"self\" (Object), outputs \"OutputDelegate\".\n"
    "Self-context delegates (no delegate_class) require a pre-compiled Blueprint.\n"
    "\n"
    "Example:\n"
    "  blueprint_compose(name=\"BP_HealthActor\", path=\"/Game/Blueprints/\",\n"
    "    variables=[{\"name\": \"Health\", \"type\": \"float\", \"default_value\": \"100.0\", \"is_exposed\": True}],\n"
    "    nodes=[{\"name\": \"BeginPlay\", \"class\": \"Event\", \"params\": {\"function_name\": \"Actor.ReceiveBeginPlay\"}},\n"
    "           {\"name\": \"PrintHealth\", \"class\": \"CallFunction\", \"params\": {\"function_name\": \"KismetSystemLibrary.PrintString\"},\n"
    "            \"pin_values\": {\"InString\": \"Actor spawned!\"}}],\n"
    "    connections=[{\"from\": \"BeginPlay.then\", \"to\": \"PrintHealth.execute\"}])\n"
)

_SAFE_UPDATE_DOC = (
    "\nSafe update mode:\n"
    "  blueprint_compose(mode='update', asset_path='/Game/BP_X.BP_X', patch={...})\n"
    "Updates other than `prune_island` and `retire_entries` use the normal reviewed patch flow. "
    "`prune_island` and `retire_entries` are complete-approval migrations: their previews must be "
    "complete and fit the response budget, and their applies require the validation hash from a "
    "reviewed preview containing the exact approved GUID set, then use a bounded preflight followed "
    "by one one-shot apply; an initial partition preview hash is not sufficient. If the apply result "
    "is ambiguous, perform readback reconciliation and re-preview before any retry.\n"
    "The MCP boundary refuses `limit`, `cursor`, `offset`, and `page` on every `graph.apply_patch` "
    "before dispatch.\n"
    "The facade adds `asset_path` and sends `patch` unchanged, so native validation remains authoritative for\n"
    "remaining patch-envelope fields:\n"
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
            "graph.apply_patch contract: updates other than `prune_island` and `retire_entries` send "
            "once, while `prune_island` and `retire_entries` applies use a bounded preflight and one "
            "one-shot apply. The legacy batch update route was removed and is never used as a fallback."
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
    _bp_doc = _BP_DISAMBIG + _CREATE_DOC + _SAFE_UPDATE_DOC

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
