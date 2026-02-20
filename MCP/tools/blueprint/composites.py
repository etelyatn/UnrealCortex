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

_VALID_BP_TYPES = {"Actor", "Component", "Interface", "FunctionLibrary"}


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
        return "$steps[" in value
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
    if bp_type == "Widget":
        raise ValueError("type 'Widget' is not supported here. Use create_widget_screen instead.")
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
        n = (node.get("name") or "").strip() or f"Node_{i}"
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
    parent_class: str = "",
) -> list[dict]:
    """Translate Blueprint spec into batch commands with $ref wiring."""
    path = path.rstrip("/")
    commands: list[dict] = []

    # Step 0: create blueprint
    create_params: dict = {"name": name, "path": path, "type": bp_type}
    if parent_class:
        create_params["parent_class"] = parent_class
    commands.append({"command": "bp.create", "params": create_params})

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
            "name": func["name"],
        }
        if "is_pure" in func:
            func_params["is_pure"] = func["is_pure"]
        if "access" in func:
            func_params["access"] = func["access"]
        commands.append({"command": "bp.add_function", "params": func_params})

    # Build node_name -> step_index map dynamically to stay correct if
    # variable/function command count ever changes conditionally.
    node_name_to_step: dict[str, int] = {}
    base_node_step = len(commands)

    # Steps base..base+N: add nodes
    for i, node in enumerate(nodes):
        node_name = (node.get("name") or "").strip() or f"Node_{i}"
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
        node_name = (node.get("name") or "").strip() or f"Node_{i}"
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


def register_blueprint_composite_tools(mcp, connection: UEConnection):
    """Register Blueprint composite MCP tools."""

    @mcp.tool()
    def create_blueprint_graph(
        name: str,
        path: str,
        type: str = "Actor",
        parent_class: str = "",
        variables: list[dict] = None,
        functions: list[dict] = None,
        graph_name: str = "EventGraph",
        nodes: list[dict] = None,
        connections: list[dict] = None,
    ) -> str:
        """Create a Blueprint with variables, functions, and graph logic in a single operation.

        Creates a new Blueprint asset, adds variables, functions, graph nodes, sets pin values,
        and wires connections. All operations execute atomically via batch.

        Use this instead of calling create_blueprint + add_blueprint_variable + graph_add_node
        individually when building a Blueprint from scratch.

        Args:
            name: Blueprint name (e.g., "BP_HealthSystem")
            path: Directory path (e.g., "/Game/Blueprints/")
            type: Blueprint base type: Actor, Component, Interface, FunctionLibrary
                  (for Widget Blueprints use create_widget_screen instead)
            parent_class: Optional C++ parent class path (e.g., "/Script/Engine.Character").
                          When set, overrides the base type selection.
            variables: Array of variable specs with:
                - name: Variable name
                - type: Variable type (float, int, bool, string, Vector, etc.)
                - default_value: Optional default value as string
                - is_exposed: Optional bool for EditAnywhere
                - category: Optional category name
            functions: Array of function specs with:
                - name: Function name
                - is_pure: Optional bool
                - access: Optional access level
            graph_name: Target graph name (default: "EventGraph")
            nodes: Array of node specs with:
                - name: Unique identifier for referencing in connections
                - class: Node class short name. Common classes:
                    Event, CustomEvent, CallFunction, Branch, Sequence,
                    VariableGet, VariableSet, SpawnActor, CastTo, ForEachLoop
                - params: Optional dict of node-specific params (e.g., {"function_name": "KismetSystemLibrary.PrintString"})
                - pin_values: Optional dict of pin default values (e.g., {"InString": "Hello!"})
            connections: Array of connection specs using "NodeName.PinName" format:
                - from: Source "NodeName.PinName" (e.g., "BeginPlay.then")
                - to: Target "NodeName.PinName" (e.g., "PrintString.execute")

                Common pin names by node type:
                    Event: outputs "then"
                    CallFunction: inputs "execute", outputs "then"
                    Branch: inputs "execute", "Condition", outputs "True", "False"
                    Sequence: inputs "execute", outputs "then 0", "then 1", ...
                    VariableGet: output is variable name
                    VariableSet: inputs "execute" + variable name, outputs "then"

        Returns:
            JSON with asset_path, node_count, variable_count, function_count, timing.
            On failure: summary, asset_path, completed_steps, failed_step, total_steps.

        Example:
            create_blueprint_graph(
                name="BP_HealthActor", path="/Game/Blueprints/",
                variables=[{"name": "Health", "type": "float", "default_value": "100.0", "is_exposed": True}],
                nodes=[
                    {"name": "BeginPlay", "class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}},
                    {"name": "PrintHealth", "class": "CallFunction",
                     "params": {"function_name": "KismetSystemLibrary.PrintString"},
                     "pin_values": {"InString": "Actor spawned!"}},
                ],
                connections=[{"from": "BeginPlay.then", "to": "PrintHealth.execute"}]
            )
        """
        variables = variables or []
        functions = functions or []
        nodes = nodes or []
        connections = connections or []

        # 1. Validate spec
        try:
            _validate_spec(name, path, bp_type=type, variables=variables,
                          functions=functions, nodes=nodes, connections=connections)
        except ValueError as e:
            return json.dumps({"success": False, "error": f"Invalid spec: {e}"})

        # 2. Build batch commands
        commands = _build_batch_commands(name, path, type, variables, functions, nodes, connections, graph_name, parent_class)
        total_steps = len(commands)

        # 3. Send batch with stop_on_error
        timeout = max(60, len(commands) * 2)
        try:
            batch_result = connection.send_command("batch", {
                "stop_on_error": True,
                "commands": commands,
            }, timeout=timeout)
        except RuntimeError as e:
            return json.dumps({"success": False, "error": str(e)})
        except (ConnectionError, TimeoutError, OSError) as e:
            return json.dumps({"success": False, "error": f"Connection error: {e}"})

        batch_data = batch_result.get("data", {})
        results = batch_data.get("results", [])

        # 4. Check for failures
        asset_path = None
        failed_step = None
        completed_count = 0

        for entry in results:
            if entry.get("success"):
                completed_count += 1
                if entry.get("index") == 0 and "data" in entry:
                    asset_path = entry["data"].get("asset_path")
            else:
                failed_step = entry
                break

        # 5. Handle failure
        if failed_step is not None:
            recovery_action = None
            if asset_path:
                try:
                    connection.send_command("bp.delete", {"asset_path": asset_path, "force": True})
                    recovery_action = {"action": "deleted_partial", "path": asset_path}
                except Exception as e:
                    recovery_action = {
                        "action": "cleanup_failed",
                        "path": asset_path,
                        "error": str(e),
                        "user_action_required": f"Manually delete partial asset at {asset_path}",
                    }

            response = {
                "success": False,
                "summary": f"Step {failed_step['index']} of {total_steps} failed: "
                           f"{failed_step.get('command', '?')} - "
                           f"{failed_step.get('error_message', 'Unknown error')}",
                "asset_path": asset_path,
                "completed_steps": completed_count,
                "failed_step": {
                    "index": failed_step["index"],
                    "command": failed_step.get("command", ""),
                    "error": failed_step.get("error_message", ""),
                },
                "total_steps": total_steps,
            }
            if recovery_action:
                response["recovery_action"] = recovery_action
            return json.dumps(response, indent=2)

        # 6. Success — post-batch non-critical steps
        warnings = []
        if asset_path and nodes:
            try:
                connection.send_command("graph.auto_layout", {
                    "asset_path": asset_path,
                    "graph_name": graph_name,
                })
            except Exception as e:
                logger.warning(f"auto_layout failed for {asset_path}: {e}", exc_info=True)
                warnings.append({
                    "step": "auto_layout",
                    "error": str(e),
                    "impact": "Node positions may need manual adjustment in editor",
                })

        if asset_path:
            try:
                connection.send_command("bp.compile", {"asset_path": asset_path})
            except (RuntimeError, ConnectionError, TimeoutError, OSError) as e:
                # Compile failure is a hard error — save the uncompiled asset so the
                # user can open it in the editor and diagnose the graph errors.
                try:
                    connection.send_command("bp.save", {"asset_path": asset_path})
                except Exception:
                    pass
                return json.dumps({
                    "success": False,
                    "error": f"Blueprint compilation failed: {e}",
                    "asset_path": asset_path,
                    "suggestion": "Asset was created but has compile errors. "
                                  "Open in Unreal Editor to diagnose graph issues, "
                                  "then fix the spec and recreate.",
                }, indent=2)

            try:
                connection.send_command("bp.save", {"asset_path": asset_path})
            except Exception as e:
                logger.warning(f"save failed for {asset_path}: {e}", exc_info=True)
                warnings.append({"step": "save", "error": str(e)})

        node_count = sum(1 for c in commands if c["command"] == "graph.add_node")
        connect_count = sum(1 for c in commands if c["command"] == "graph.connect")
        pin_count = sum(1 for c in commands if c["command"] == "graph.set_pin_value")

        response = {
            "success": True,
            "asset_path": asset_path,
            "total_steps": total_steps,
            "variable_count": len(variables),
            "function_count": len(functions),
            "node_count": node_count,
            "connection_count": connect_count,
            "pin_values_set": pin_count,
            "total_timing_ms": batch_data.get("total_timing_ms", 0),
        }
        if warnings:
            response["warnings"] = warnings

        return json.dumps(response, indent=2)
