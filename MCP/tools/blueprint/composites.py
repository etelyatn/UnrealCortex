"""MCP composite tools for high-level Blueprint graph creation."""

import json
import logging
from typing import Any
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

# Short class name -> full UE class name
_BP_CLASS_MAP = {
    "Event": "UK2Node_Event",
    "CustomEvent": "UK2Node_CustomEvent",
    "CallFunction": "UK2Node_CallFunction",
    "IfThenElse": "UK2Node_IfThenElse",
    "Branch": "UK2Node_IfThenElse",
    "Sequence": "UK2Node_ExecutionSequence",
    "VariableGet": "UK2Node_VariableGet",
    "VariableSet": "UK2Node_VariableSet",
    "FunctionEntry": "UK2Node_FunctionEntry",
    "FunctionResult": "UK2Node_FunctionResult",
    "Self": "UK2Node_Self",
    "Knot": "UK2Node_Knot",
    "MakeArray": "UK2Node_MakeArray",
    "Timeline": "UK2Node_Timeline",
    "SpawnActor": "UK2Node_SpawnActorFromClass",
    "CastTo": "UK2Node_DynamicCast",
    "ForEachLoop": "UK2Node_MacroInstance",
    "SwitchEnum": "UK2Node_SwitchEnum",
    "SwitchString": "UK2Node_SwitchString",
    "SwitchInteger": "UK2Node_SwitchInteger",
}

_VALID_BP_TYPES = {"Actor", "Component", "Widget", "Interface", "FunctionLibrary"}


def _resolve_class_name(short_name: str) -> str:
    """Resolve short class name to full UE K2Node class name."""
    if short_name in _BP_CLASS_MAP:
        return _BP_CLASS_MAP[short_name]
    if short_name.startswith("UK2Node_"):
        return short_name
    return f"UK2Node_{short_name}"


def _contains_ref_syntax(value):
    """Check if value or any nested element contains $steps[ syntax."""
    if isinstance(value, str):
        return value.startswith("$steps[")
    elif isinstance(value, list):
        return any(_contains_ref_syntax(v) for v in value)
    elif isinstance(value, dict):
        return any(_contains_ref_syntax(v) for v in value.values())
    return False


def _validate_spec(
    name: str,
    path: str,
    bp_type: str = "Actor",
    variables: list[dict] | None = None,
    functions: list[dict] | None = None,
    nodes: list[dict] | None = None,
    connections: list[dict] | None = None,
):
    """Validate the Blueprint graph spec. Raises ValueError on invalid spec."""
    if not name:
        raise ValueError("Missing required field: name")
    if not path:
        raise ValueError("Missing required field: path")
    if bp_type not in _VALID_BP_TYPES:
        raise ValueError(f"type must be one of {sorted(_VALID_BP_TYPES)}")

    variables = variables or []
    functions = functions or []
    nodes = nodes or []
    connections = connections or []

    # Variable name uniqueness
    var_names = [v.get("name", "") for v in variables]
    if len(var_names) != len(set(var_names)):
        raise ValueError("Duplicate variable names in spec")

    # Function name uniqueness
    func_names = [f.get("name", "") for f in functions]
    if len(func_names) != len(set(func_names)):
        raise ValueError("Duplicate function names in spec")

    # Node validation
    node_names = []
    for i, node in enumerate(nodes):
        if "class" not in node:
            raise ValueError(f"Node at index {i} missing 'class' field")
        n = node.get("name", "").strip()
        if not n:
            n = f"Node_{i}"
            node["name"] = n
        node_names.append(n)

    if len(node_names) != len(set(node_names)):
        raise ValueError("Duplicate node names in spec")

    # Connection validation
    node_name_set = set(node_names)
    for conn in connections:
        if "from" not in conn or "to" not in conn:
            raise ValueError("Connection missing 'from' or 'to' field")

        src_parts = conn["from"].split(".", 1)
        tgt_parts = conn["to"].split(".", 1)
        if len(src_parts) != 2:
            raise ValueError(f"Invalid 'from' format: {conn['from']} (expected 'NodeName.PinName')")
        if len(tgt_parts) != 2:
            raise ValueError(f"Invalid 'to' format: {conn['to']} (expected 'NodeName.PinName')")

        if src_parts[0] not in node_name_set:
            raise ValueError(f"Unknown source node: {src_parts[0]}")
        if tgt_parts[0] not in node_name_set:
            raise ValueError(f"Unknown target node: {tgt_parts[0]}")

    # No $steps[ in user params or pin_values
    for node in nodes:
        for key, value in (node.get("params") or {}).items():
            if _contains_ref_syntax(value):
                raise ValueError(
                    f"Node '{node.get('name')}' param '{key}' contains '$steps[' "
                    f"which conflicts with batch $ref syntax"
                )
        for key, value in (node.get("pin_values") or {}).items():
            if _contains_ref_syntax(value):
                raise ValueError(
                    f"Node '{node.get('name')}' pin_value '{key}' contains '$steps[' "
                    f"which conflicts with batch $ref syntax"
                )
