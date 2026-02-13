"""MCP composite tools for high-level material graph creation."""

import json
import logging
from typing import Any
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

# Short class name → full UE class name
_CLASS_MAP = {
    "TextureCoordinate": "MaterialExpressionTextureCoordinate",
    "TextureSample": "MaterialExpressionTextureSample",
    "TextureObject": "MaterialExpressionTextureObject",
    "ScalarParameter": "MaterialExpressionScalarParameter",
    "VectorParameter": "MaterialExpressionVectorParameter",
    "TextureParameter": "MaterialExpressionTextureSampleParameter2D",
    "StaticSwitchParameter": "MaterialExpressionStaticSwitchParameter",
    "StaticBoolParameter": "MaterialExpressionStaticBoolParameter",
    "Constant": "MaterialExpressionConstant",
    "Constant2Vector": "MaterialExpressionConstant2Vector",
    "Constant3Vector": "MaterialExpressionConstant3Vector",
    "Constant4Vector": "MaterialExpressionConstant4Vector",
    "Multiply": "MaterialExpressionMultiply",
    "Add": "MaterialExpressionAdd",
    "Subtract": "MaterialExpressionSubtract",
    "Divide": "MaterialExpressionDivide",
    "Lerp": "MaterialExpressionLinearInterpolate",
    "Power": "MaterialExpressionPower",
    "Clamp": "MaterialExpressionClamp",
    "OneMinus": "MaterialExpressionOneMinus",
    "Abs": "MaterialExpressionAbs",
    "Sine": "MaterialExpressionSine",
    "Cosine": "MaterialExpressionCosine",
    "Floor": "MaterialExpressionFloor",
    "Ceil": "MaterialExpressionCeil",
    "Frac": "MaterialExpressionFrac",
    "Fresnel": "MaterialExpressionFresnel",
    "Panner": "MaterialExpressionPanner",
    "Time": "MaterialExpressionTime",
    "WorldPosition": "MaterialExpressionWorldPosition",
    "VertexColor": "MaterialExpressionVertexColor",
    "ComponentMask": "MaterialExpressionComponentMask",
    "AppendVector": "MaterialExpressionAppendVector",
    "DesaturationFunction": "MaterialExpressionDesaturation",
    "Desaturation": "MaterialExpressionDesaturation",
    "Noise": "MaterialExpressionNoise",
    "DotProduct": "MaterialExpressionDotProduct",
    "CrossProduct": "MaterialExpressionCrossProduct",
    "Normalize": "MaterialExpressionNormalize",
    "If": "MaterialExpressionIf",
}


def _resolve_class_name(short_name: str) -> str:
    """Resolve short class name to full UE class name."""
    if short_name in _CLASS_MAP:
        return _CLASS_MAP[short_name]
    # If it already looks like a full name, pass through
    if short_name.startswith("MaterialExpression"):
        return short_name
    # Try with prefix
    prefixed = f"MaterialExpression{short_name}"
    return prefixed


def _contains_ref_syntax(value):
    """Check if value or any nested element contains $steps[ syntax."""
    if isinstance(value, str):
        return value.startswith("$steps[")
    elif isinstance(value, list):
        return any(_contains_ref_syntax(v) for v in value)
    elif isinstance(value, dict):
        return any(_contains_ref_syntax(v) for v in value.values())
    return False


def _validate_spec(name, path, nodes, connections):
    """Validate the material graph spec. Raises ValueError on invalid spec."""
    if not name:
        raise ValueError("Missing required field: name")
    if not path:
        raise ValueError("Missing required field: path")
    if not isinstance(nodes, list):
        raise ValueError("nodes must be an array")
    if not isinstance(connections, list):
        raise ValueError("connections must be an array")

    # Node name uniqueness with normalization
    node_names = []
    for i, node in enumerate(nodes):
        n = node.get("name", "").strip()
        if not n:
            n = f"Node_{i}"
            node["name"] = n  # Normalize in-place
        node_names.append(n)

    if len(node_names) != len(set(node_names)):
        raise ValueError("Duplicate node names in spec")

    # Connection validation
    for conn in connections:
        if "from" not in conn or "to" not in conn:
            raise ValueError("Connection missing 'from' or 'to' field")

        src_parts = conn["from"].split(".", 1)
        tgt_parts = conn["to"].split(".", 1)
        if len(src_parts) != 2:
            raise ValueError(f"Invalid 'from' format: {conn['from']} (expected 'NodeName.PinName')")
        if len(tgt_parts) != 2:
            raise ValueError(f"Invalid 'to' format: {conn['to']} (expected 'NodeName.PinName')")

        src_node = src_parts[0]
        tgt_node = tgt_parts[0]

        if src_node not in node_names:
            raise ValueError(f"Unknown source node: {src_node}")
        if tgt_node not in node_names and tgt_node != "Material":
            raise ValueError(f"Unknown target node: {tgt_node}")

    # Validate no user param contains $steps[ (including nested values)
    for node in nodes:
        for key, value in (node.get("params") or {}).items():
            if _contains_ref_syntax(value):
                raise ValueError(
                    f"Node '{node.get('name')}' param '{key}' contains '$steps[' "
                    f"which conflicts with batch $ref syntax"
                )


def _build_batch_commands(name, path, nodes, connections):
    """Translate material spec into batch commands with $ref wiring."""
    commands = []

    # Step 0: create material
    commands.append({
        "command": "material.create_material",
        "params": {"name": name, "asset_path": path},
    })

    # Map node name → step index (for $ref wiring)
    node_step_map = {}

    # Steps 1..N: add nodes
    for i, node in enumerate(nodes):
        node_name = node.get("name", f"Node_{i}")
        expression_class = _resolve_class_name(node["class"])

        add_params = {
            "asset_path": "$steps[0].data.asset_path",
            "expression_class": expression_class,
        }

        step_index = len(commands)
        node_step_map[node_name] = step_index

        commands.append({
            "command": "material.add_node",
            "params": add_params,
        })

    # Steps N+1..M: set node properties (if any)
    for i, node in enumerate(nodes):
        node_name = node.get("name", f"Node_{i}")
        params = node.get("params")
        if not params:
            continue

        step_index = node_step_map[node_name]
        for key, value in params.items():
            commands.append({
                "command": "material.set_node_property",
                "params": {
                    "asset_path": "$steps[0].data.asset_path",
                    "node_id": f"$steps[{step_index}].data.node_id",
                    "property_name": key,
                    "value": value,
                },
            })

    # Steps M+1..K: connections
    for conn in connections:
        src_parts = conn["from"].split(".", 1)
        tgt_parts = conn["to"].split(".", 1)
        src_node_name = src_parts[0]
        src_pin = src_parts[1]
        tgt_node_name = tgt_parts[0]
        tgt_pin = tgt_parts[1]

        connect_params = {
            "asset_path": "$steps[0].data.asset_path",
            "source_node": f"$steps[{node_step_map[src_node_name]}].data.node_id",
            "source_output": src_pin,
        }

        if tgt_node_name == "Material":
            connect_params["target_node"] = "MaterialResult"
            connect_params["target_input"] = tgt_pin
        else:
            connect_params["target_node"] = f"$steps[{node_step_map[tgt_node_name]}].data.node_id"
            connect_params["target_input"] = tgt_pin

        commands.append({
            "command": "material.connect",
            "params": connect_params,
        })

    return commands


def register_material_composite_tools(mcp, connection: UEConnection):
    """Register material composite MCP tools."""

    @mcp.tool()
    def create_material_graph(
        name: str,
        path: str,
        nodes: list[dict],
        connections: list[dict],
    ) -> str:
        """Create a material with expression nodes and connections in a single operation.

        Creates a new UMaterial asset, adds all specified expression nodes, sets their
        properties, and wires connections between them. All operations execute atomically
        via batch - if any step fails, execution stops and returns what completed.

        Use this instead of calling create_material + add_material_node + connect_material_nodes
        individually when building a material graph from scratch.

        Args:
            name: Material name (should start with M_)
            path: Directory path (e.g., "/Game/Materials/")
            nodes: Array of node specs, each with:
                - class: Expression class short name. Common classes:
                    TextureCoordinate, TextureSample, ScalarParameter, VectorParameter,
                    TextureParameter, Constant, Constant3Vector, Multiply, Add, Lerp,
                    Power, Clamp, OneMinus, Fresnel, Panner, Time, WorldPosition
                - name: Unique identifier for referencing in connections
                - params: Optional dict of node properties (e.g., {"UTiling": 2.0},
                          {"Texture": "/Game/T_Wood_D"}, {"ParameterName": "Roughness"})
            connections: Array of connection specs using "NodeName.PinName" format:
                - from: Source "NodeName.PinName" (e.g., "UV.UV", "Diffuse.RGBA")
                - to: Target "NodeName.PinName" or "Material.InputName"
                      Material inputs: BaseColor, Normal, Metallic, Roughness,
                      Specular, EmissiveColor, Opacity, OpacityMask, WorldPositionOffset

        Returns:
            JSON with asset_path, node_count, connection_count, timing.
            On failure: summary, asset_path, completed_steps, failed_step, total_steps.

        Example:
            create_material_graph(
                name="M_WoodFloor",
                path="/Game/Materials/",
                nodes=[
                    {"class": "TextureCoordinate", "name": "UV", "params": {"UTiling": 2.0, "VTiling": 2.0}},
                    {"class": "TextureSample", "name": "Diffuse", "params": {"Texture": "/Game/T_Wood_D"}},
                    {"class": "TextureSample", "name": "Normal", "params": {"Texture": "/Game/T_Wood_N"}},
                ],
                connections=[
                    {"from": "UV.UV", "to": "Diffuse.UVs"},
                    {"from": "UV.UV", "to": "Normal.UVs"},
                    {"from": "Diffuse.RGBA", "to": "Material.BaseColor"},
                    {"from": "Normal.RGBA", "to": "Material.Normal"},
                ]
            )
        """
        # 1. Validate spec
        try:
            _validate_spec(name, path, nodes, connections)
        except ValueError as e:
            return json.dumps({
                "success": False,
                "error": f"Invalid spec: {e}",
            })

        # 2. Build batch commands
        commands = _build_batch_commands(name, path, nodes, connections)
        total_steps = len(commands)

        # 3. Send batch with stop_on_error
        # Timeout scaling: ~0.5s per add_node, ~1s per set_property, ~0.3s per connect
        # Conservative 2s average per command with 60s minimum for small graphs
        timeout = max(60, len(commands) * 2)
        try:
            batch_result = connection.send_command("batch", {
                "stop_on_error": True,
                "commands": commands,
            }, timeout=timeout)
        except RuntimeError as e:
            # Batch itself failed (e.g., limit exceeded)
            return json.dumps({"success": False, "error": str(e)})
        except (ConnectionError, TimeoutError, OSError) as e:
            # Connection lost or timeout exceeded
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
            # Cleanup partial asset if step 0 succeeded
            recovery_action = None
            if asset_path:
                try:
                    connection.send_command("material.delete_material", {
                        "asset_path": asset_path,
                    })
                    recovery_action = {"action": "deleted_partial", "path": asset_path}
                except Exception as e:
                    recovery_action = {
                        "action": "cleanup_failed",
                        "path": asset_path,
                        "error": str(e),
                        "user_action_required": f"Manually delete partial asset at {asset_path}"
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

        # 6. Success — call auto_layout as separate post-batch call
        warnings = []
        if asset_path:
            try:
                connection.send_command("material.auto_layout", {
                    "asset_path": asset_path,
                })
            except Exception as e:
                logger.warning(f"auto_layout failed for {asset_path}: {e}", exc_info=True)
                warnings.append({
                    "step": "auto_layout",
                    "error": str(e),
                    "impact": "Node positions may need manual adjustment in editor"
                })

        # Count operations by type
        node_count = sum(1 for c in commands if c["command"] == "material.add_node")
        connect_count = sum(1 for c in commands if c["command"] == "material.connect")
        prop_count = sum(1 for c in commands if c["command"] == "material.set_node_property")

        response = {
            "success": True,
            "asset_path": asset_path,
            "total_steps": total_steps,
            "node_count": node_count,
            "connection_count": connect_count,
            "properties_set": prop_count,
            "total_timing_ms": batch_data.get("total_timing_ms", 0),
            "steps_summary": {
                "create": 1,
                "add_node": node_count,
                "set_node_property": prop_count,
                "connect": connect_count,
                "auto_layout": 1,
            },
        }
        if warnings:
            response["warnings"] = warnings

        return json.dumps(response, indent=2)
