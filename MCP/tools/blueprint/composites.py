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


def _build_batch_commands(
    name: str,
    path: str,
    bp_type: str,
    variables: list[dict],
    functions: list[dict],
    nodes: list[dict],
    connections: list[dict],
    graph_name: str,
) -> list[dict]:
    """Translate Blueprint spec into batch commands with $ref wiring."""
    path = path.rstrip("/")
    commands: list[dict] = []

    # Step 0: create blueprint
    commands.append({
        "command": "bp.create",
        "params": {"name": name, "path": path, "type": bp_type},
    })

    # Steps 1..V: add variables
    for var in variables:
        var_params: dict[str, Any] = {
            "asset_path": "$steps[0].data.asset_path",
            "variable_name": var["name"],
            "variable_type": var.get("type", "bool"),
        }
        if "default_value" in var:
            var_params["default_value"] = var["default_value"]
        if "is_exposed" in var:
            var_params["is_exposed"] = var["is_exposed"]
        if "category" in var:
            var_params["category"] = var["category"]
        commands.append({"command": "bp.add_variable", "params": var_params})

    # Steps V+1..V+F: add functions
    for func in functions:
        func_params: dict[str, Any] = {
            "asset_path": "$steps[0].data.asset_path",
            "function_name": func["name"],
        }
        if "is_pure" in func:
            func_params["is_pure"] = func["is_pure"]
        if "access" in func:
            func_params["access"] = func["access"]
        commands.append({"command": "bp.add_function", "params": func_params})

    # Build node_name -> step_index map
    # node_step_index = 1 + len(variables) + len(functions) + node_index
    node_name_to_step: dict[str, int] = {}
    base_node_step = 1 + len(variables) + len(functions)

    # Steps base..base+N: add nodes
    for i, node in enumerate(nodes):
        node_name = node.get("name", f"Node_{i}")
        step_index = base_node_step + i
        node_name_to_step[node_name] = step_index

        add_params: dict[str, Any] = {
            "asset_path": "$steps[0].data.asset_path",
            "node_class": _resolve_class_name(node["class"]),
            "graph_name": graph_name,
        }
        if node.get("params"):
            add_params["params"] = node["params"]
        commands.append({"command": "graph.add_node", "params": add_params})

    # Steps: set pin values
    for i, node in enumerate(nodes):
        node_name = node.get("name", f"Node_{i}")
        pin_values = node.get("pin_values")
        if not pin_values:
            continue
        step_index = node_name_to_step[node_name]
        for pin_name, value in pin_values.items():
            commands.append({
                "command": "graph.set_pin_value",
                "params": {
                    "asset_path": "$steps[0].data.asset_path",
                    "node_id": f"$steps[{step_index}].data.node_id",
                    "graph_name": graph_name,
                    "pin_name": pin_name,
                    "value": value,
                },
            })

    # Steps: connections
    for conn in connections:
        src_parts = conn["from"].split(".", 1)
        tgt_parts = conn["to"].split(".", 1)
        commands.append({
            "command": "graph.connect",
            "params": {
                "asset_path": "$steps[0].data.asset_path",
                "source_node": f"$steps[{node_name_to_step[src_parts[0]]}].data.node_id",
                "source_pin": src_parts[1],
                "target_node": f"$steps[{node_name_to_step[tgt_parts[0]]}].data.node_id",
                "target_pin": tgt_parts[1],
                "graph_name": graph_name,
            },
        })

    return commands
