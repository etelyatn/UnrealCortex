"""MCP composite tools for high-level material graph creation."""

import json
import logging
import re
from typing import Any
from cortex_mcp.verification import VERIFICATION_TIMEOUT
from cortex_mcp.verification.material import verify_material
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


# Known pin names for common expression types.
# outputs: list of output pin names (use index 0 if empty/single)
# inputs: list of input pin names
_PIN_MAP = {
    # Parameters — single unnamed output (use index 0)
    "ScalarParameter": {"outputs": ["0"], "inputs": []},
    "VectorParameter": {"outputs": ["0"], "inputs": []},
    "TextureParameter": {"outputs": ["RGBA", "RGB", "R", "G", "B", "A"], "inputs": ["UVs"]},
    "StaticSwitchParameter": {"outputs": ["0"], "inputs": ["True", "False", "Value"]},
    # Constants
    "Constant": {"outputs": ["0"], "inputs": []},
    "Constant2Vector": {"outputs": ["0"], "inputs": []},
    "Constant3Vector": {"outputs": ["0"], "inputs": []},
    "Constant4Vector": {"outputs": ["0"], "inputs": []},
    # Math — two inputs (A, B), single output
    "Multiply": {"outputs": ["0"], "inputs": ["A", "B"]},
    "Add": {"outputs": ["0"], "inputs": ["A", "B"]},
    "Subtract": {"outputs": ["0"], "inputs": ["A", "B"]},
    "Divide": {"outputs": ["0"], "inputs": ["A", "B"]},
    "Power": {"outputs": ["0"], "inputs": ["Base", "Exp"]},
    "DotProduct": {"outputs": ["0"], "inputs": ["A", "B"]},
    "CrossProduct": {"outputs": ["0"], "inputs": ["A", "B"]},
    # Math — single input
    "OneMinus": {"outputs": ["0"], "inputs": ["Input"]},
    "Abs": {"outputs": ["0"], "inputs": ["Input"]},
    "Sine": {"outputs": ["0"], "inputs": ["Input"]},
    "Cosine": {"outputs": ["0"], "inputs": ["Input"]},
    "Floor": {"outputs": ["0"], "inputs": ["Input"]},
    "Ceil": {"outputs": ["0"], "inputs": ["Input"]},
    "Frac": {"outputs": ["0"], "inputs": ["Input"]},
    "Normalize": {"outputs": ["0"], "inputs": ["VectorInput"]},
    # Interpolation
    "Lerp": {"outputs": ["0"], "inputs": ["A", "B", "Alpha"]},
    "Clamp": {"outputs": ["0"], "inputs": ["Input", "Min", "Max"]},
    "If": {"outputs": ["0"], "inputs": ["A", "B", "AGreaterThanB", "AEqualsB", "ALessThanB"]},
    # Texture
    "TextureCoordinate": {"outputs": ["0"], "inputs": []},
    "TextureSample": {"outputs": ["RGBA", "RGB", "R", "G", "B", "A"], "inputs": ["UVs", "Tex"]},
    "TextureObject": {"outputs": ["0"], "inputs": []},
    # Animation / Time
    "Time": {"outputs": ["0"], "inputs": []},
    "Panner": {"outputs": ["0"], "inputs": ["Coordinate", "Time", "Speed", "SpeedX", "SpeedY"]},
    # World
    "WorldPosition": {"outputs": ["0"], "inputs": []},
    "VertexColor": {"outputs": ["RGBA", "RGB", "R", "G", "B", "A"], "inputs": []},
    # Vector ops
    "ComponentMask": {"outputs": ["0"], "inputs": ["Input"]},
    "AppendVector": {"outputs": ["0"], "inputs": ["A", "B"]},
    "Desaturation": {"outputs": ["0"], "inputs": ["Input", "Fraction"]},
    # Fresnel
    "Fresnel": {"outputs": ["0"], "inputs": ["ExponentIn", "BaseReflectFractionIn", "Normal"]},
    # Noise
    "Noise": {"outputs": ["0"], "inputs": ["Position"]},
    # LinearInterpolate (alias for Lerp — full UE class name variant)
    "LinearInterpolate": {"outputs": ["0"], "inputs": ["A", "B", "Alpha"]},
}


def _to_snake_case(name: str) -> str:
    """Convert common material property names to snake_case for verifier checks."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


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


def _validate_spec(name, path, nodes, connections, material_properties=None):
    """Validate the material graph spec. Raises ValueError on invalid spec."""
    if not name:
        raise ValueError("Missing required field: name")
    if not path:
        raise ValueError("Missing required field: path")
    if material_properties is not None and not isinstance(material_properties, dict):
        raise ValueError("material_properties must be a dict")
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

    # Build node name → class map for pin validation
    node_class_map = {}
    for node in nodes:
        n = node.get("name", "").strip()
        cls = node.get("class", "")
        # Normalize class name to short form for _PIN_MAP lookup
        short_cls = cls
        if short_cls.startswith("MaterialExpression"):
            short_cls = short_cls[len("MaterialExpression"):]
        node_class_map[n] = short_cls

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
        src_pin = src_parts[1]
        tgt_node = tgt_parts[0]
        tgt_pin = tgt_parts[1]

        if src_node not in node_names:
            raise ValueError(f"Unknown source node: {src_node}")
        if tgt_node not in node_names and tgt_node != "Material":
            raise ValueError(f"Unknown target node: {tgt_node}")

        # Validate pin names against _PIN_MAP (if class is known)
        src_cls = node_class_map.get(src_node, "")
        if src_cls in _PIN_MAP:
            known_outputs = _PIN_MAP[src_cls]["outputs"]
            if known_outputs and src_pin not in known_outputs:
                raise ValueError(
                    f"Unknown output pin '{src_pin}' on {src_node} ({src_cls}). "
                    f"Available outputs: {known_outputs}"
                )

        if tgt_node == "Material":
            valid_material_inputs = [
                "BaseColor", "Normal", "Metallic", "Roughness", "Specular",
                "EmissiveColor", "Opacity", "OpacityMask", "WorldPositionOffset",
            ]
            if tgt_pin not in valid_material_inputs:
                raise ValueError(
                    f"Unknown Material input '{tgt_pin}'. "
                    f"Available inputs: {valid_material_inputs}"
                )
        else:
            tgt_cls = node_class_map.get(tgt_node, "")
            if tgt_cls in _PIN_MAP:
                known_inputs = _PIN_MAP[tgt_cls]["inputs"]
                if known_inputs and tgt_pin not in known_inputs:
                    raise ValueError(
                        f"Unknown input pin '{tgt_pin}' on {tgt_node} ({tgt_cls}). "
                        f"Available inputs: {known_inputs}"
                    )

    # Validate no user param contains $steps[ (including nested values)
    for node in nodes:
        for key, value in (node.get("params") or {}).items():
            if _contains_ref_syntax(value):
                raise ValueError(
                    f"Node '{node.get('name')}' param '{key}' contains '$steps[' "
                    f"which conflicts with batch $ref syntax"
                )


_VALID_PARAM_TYPES = {"scalar", "vector", "texture"}

# Node class -> parameter type mapping for type inference.
_PARAM_TYPE_MAP = {
    "ScalarParameter": "scalar",
    "MaterialExpressionScalarParameter": "scalar",
    "VectorParameter": "vector",
    "MaterialExpressionVectorParameter": "vector",
    "TextureParameter": "texture",
    "MaterialExpressionTextureSampleParameter2D": "texture",
    "TextureSampleParameter2D": "texture",
    "TextureObjectParameter": "texture",
    "MaterialExpressionTextureObjectParameter": "texture",
}


def _infer_param_type(node_class):
    """Infer parameter type from node class name."""
    return _PARAM_TYPE_MAP.get(node_class)


def _validate_instance_spec(name, path, parent, parameters):
    """Validate the material instance spec. Raises ValueError on invalid spec."""
    if not name:
        raise ValueError("Missing required field: name")
    if not path:
        raise ValueError("Missing required field: path")
    if not parent:
        raise ValueError("Missing required field: parent")
    if not isinstance(parameters, list):
        raise ValueError("parameters must be a list")

    seen_names = set()
    for i, param in enumerate(parameters):
        if not isinstance(param, dict):
            raise ValueError(f"Parameter {i} must be a dict")
        if "name" not in param or not param["name"]:
            raise ValueError(f"Parameter {i} missing 'name'")
        if "type" not in param or not param["type"]:
            raise ValueError(f"Parameter {i} missing 'type'")
        if "value" not in param:
            raise ValueError(f"Parameter {i} missing 'value'")
        if param["type"] not in _VALID_PARAM_TYPES:
            raise ValueError(
                f"Invalid parameter type '{param['type']}' for '{param['name']}'. "
                f"Must be one of: {sorted(_VALID_PARAM_TYPES)}"
            )
        if param["name"] in seen_names:
            raise ValueError(f"Duplicate parameter name: '{param['name']}'")
        seen_names.add(param["name"])


def _build_batch_commands(name, path, nodes, connections, material_properties=None):
    """Translate material spec into batch commands with $ref wiring."""
    # Normalize trailing slash to prevent double-slash paths
    path = path.rstrip("/")

    commands = []

    # Step 0: create material
    commands.append({
        "command": "material.create_material",
        "params": {"name": name, "asset_path": path},
    })

    # Steps 1..P: set material-level properties (before adding nodes)
    if material_properties:
        for prop_name, prop_value in material_properties.items():
            commands.append({
                "command": "material.set_material_property",
                "params": {
                    "asset_path": "$steps[0].data.asset_path",
                    "property_name": prop_name,
                    "value": prop_value,
                },
            })

    # Map node name → step index (for $ref wiring)
    node_step_map = {}

    # Steps P+1..N: add nodes
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


def _build_instance_batch_commands(name, path, parent, parameters, parent_ref=None, step_offset=0):
    """Translate instance spec into batch commands with $ref wiring.

    Args:
        name: Instance name.
        path: Directory path for the instance.
        parent: Parent material asset path (used when parent_ref is None).
        parameters: List of parameter overrides [{name, type, value}].
        parent_ref: Optional $steps[N] reference to use as parent_material.
        step_offset: Step index where this instance's commands start in merged batch.

    Returns:
        List of batch command dicts.
    """
    path = path.rstrip("/")

    commands = []

    commands.append({
        "command": "material.create_instance",
        "params": {
            "name": name,
            "asset_path": path,
            "parent_material": parent_ref if parent_ref else parent,
        },
    })

    if parameters:
        params_list = [
            {
                "parameter_name": p["name"],
                "parameter_type": p["type"],
                "value": p["value"],
            }
            for p in parameters
        ]
        commands.append({
            "command": "material.set_parameters",
            "params": {
                "asset_path": f"$steps[{step_offset}].data.asset_path",
                "parameters": params_list,
            },
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
        material_properties: dict | None = None,
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
                - from: Source "NodeName.OutputPin" (e.g., "MyParam.0", "Diffuse.RGBA")
                - to: Target "NodeName.InputPin" or "Material.InputName"

                Output pin conventions (source_output):
                    Most nodes have a single output — use "0" (e.g., "Time.0", "Multiply.0")
                    Texture nodes have named outputs: "RGBA", "RGB", "R", "G", "B", "A"

                Input pin conventions (target_input):
                    Math ops (Multiply, Add, Subtract, Divide): "A", "B"
                    Single-input ops (Sine, Cosine, Abs, OneMinus, Floor, Ceil, Frac): "Input"
                    Lerp/LinearInterpolate: "A", "B", "Alpha"
                    Clamp: "Input", "Min", "Max"
                    Power: "Base", "Exp"
                    TextureSample: "UVs", "Tex"
                    Panner: "Coordinate", "Time", "Speed"
                    Fresnel: "ExponentIn", "BaseReflectFractionIn", "Normal"

                Material result inputs: BaseColor, Normal, Metallic, Roughness,
                    Specular, EmissiveColor, Opacity, OpacityMask, WorldPositionOffset
            material_properties: Optional dict of material-level properties to set.
                Keys are property names, values are the property values.
                Applied after material creation, before adding nodes.
                Example: {"BlendMode": "BLEND_Translucent", "ShadingModel": "MSM_Unlit"}

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
            _validate_spec(name, path, nodes, connections, material_properties)
        except ValueError as e:
            return json.dumps({
                "success": False,
                "error": f"Invalid spec: {e}",
            })

        # 2. Build batch commands
        commands = _build_batch_commands(name, path, nodes, connections, material_properties)
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
        mat_prop_count = sum(1 for c in commands if c["command"] == "material.set_material_property")

        response = {
            "success": True,
            "asset_path": asset_path,
            "total_steps": total_steps,
            "node_count": node_count,
            "connection_count": connect_count,
            "properties_set": prop_count,
            "material_properties_set": mat_prop_count,
            "total_timing_ms": batch_data.get("total_timing_ms", 0),
            "steps_summary": {
                "create": 1,
                "set_material_property": mat_prop_count,
                "add_node": node_count,
                "set_node_property": prop_count,
                "connect": connect_count,
                "auto_layout": 1,
            },
        }

        # Verification readback is informational. It must not downgrade successful creation.
        try:
            mat_data = connection.send_command(
                "material.get_material",
                {"asset_path": asset_path},
                timeout=VERIFICATION_TIMEOUT,
            )
            nodes_data = connection.send_command(
                "material.list_nodes",
                {"asset_path": asset_path},
                timeout=VERIFICATION_TIMEOUT,
            )
            connections_data = connection.send_command(
                "material.list_connections",
                {"asset_path": asset_path},
                timeout=VERIFICATION_TIMEOUT,
            )
            mat_result = mat_data.get("data", mat_data)
            readback = {
                "node_count": mat_result.get("node_count", 0),
                "blend_mode": mat_result.get("blend_mode"),
                "shading_model": mat_result.get("shading_model"),
                "nodes": nodes_data.get("data", nodes_data).get("nodes", []),
                "connections": connections_data.get("data", connections_data).get("connections", []),
            }
            verification_spec = {
                "nodes": nodes,
                "connections": connections,
                "material_properties": {
                    _to_snake_case(key): value for key, value in (material_properties or {}).items()
                },
            }
            verification_result = verify_material(verification_spec, readback)
            response["verification"] = verification_result.to_dict()
            if verification_result.verified is False:
                failed_count = sum(1 for check in verification_result.checks.values() if not check.passed)
                response["warning"] = (
                    f"Verification failed: {failed_count} of {len(verification_result.checks)} "
                    "checks did not pass"
                )
        except Exception as exc:
            logger.warning("Material verification readback failed: %s", exc, exc_info=True)
            response["verification"] = {
                "verified": None,
                "error_code": "READBACK_FAILED",
                "error": f"Verification readback failed: {exc}",
            }

        if warnings:
            response["warnings"] = warnings

        return json.dumps(response, indent=2)

    @mcp.tool()
    def create_material_instance(
        name: str,
        path: str,
        parent: str,
        parameters: list[dict] | None = None,
    ) -> str:
        """Create a material instance with parameter overrides in a single operation.

        Creates a new UMaterialInstanceConstant and sets all specified parameter
        overrides. All operations execute atomically via batch - if any step fails,
        the partial instance is cleaned up.

        Args:
            name: Instance name.
            path: Directory path.
            parent: Parent material asset path.
            parameters: Optional array of parameter overrides with {name,type,value}.

        Returns:
            JSON result including asset_path, parent_material, and parameter_count.
            On failure returns error summary and failed step info.
        """
        params = parameters or []

        try:
            _validate_instance_spec(name, path, parent, params)
        except ValueError as e:
            return json.dumps({"success": False, "error": f"Invalid spec: {e}"})

        commands = _build_instance_batch_commands(name, path, parent, params)
        total_steps = len(commands)
        timeout = max(30, total_steps * 2)

        try:
            batch_result = connection.send_command("batch", {
                "stop_on_error": True,
                "commands": commands,
            }, timeout=timeout)
        except RuntimeError as e:
            return json.dumps({"success": False, "error": str(e)})
        except (ConnectionError, TimeoutError, OSError) as e:
            return json.dumps({"success": False, "error": f"Connection error: {e}"})

        if not batch_result.get("success", False):
            return json.dumps({
                "success": False,
                "error": batch_result.get("error", "Batch execution failed"),
            })

        batch_data = batch_result.get("data", {})
        results = batch_data.get("results", [])

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

        if failed_step is not None:
            recovery_action = None
            if asset_path:
                try:
                    connection.send_command("material.delete_instance", {
                        "asset_path": asset_path,
                    })
                    recovery_action = {"action": "deleted_partial", "path": asset_path}
                except Exception as e:
                    recovery_action = {
                        "action": "cleanup_failed",
                        "path": asset_path,
                        "error": str(e),
                        "user_action_required": f"Manually delete partial instance at {asset_path}",
                    }

            response = {
                "success": False,
                "summary": f"Step {failed_step['index']} of {total_steps} failed: "
                           f"{failed_step.get('command', '?')} - "
                           f"{failed_step.get('error_message', 'Unknown error')}",
                "asset_path": asset_path,
                "parent_material": parent,
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

        response = {
            "success": True,
            "asset_path": asset_path,
            "parent_material": parent,
            "parameter_count": len(params),
            "total_steps": total_steps,
            "total_timing_ms": batch_data.get("total_timing_ms", 0),
        }

        try:
            instance_data = connection.send_command(
                "material.get_instance",
                {"asset_path": asset_path},
                timeout=VERIFICATION_TIMEOUT,
            )
            inst_result = instance_data.get("data", instance_data)
            overrides = inst_result.get("overrides", {})
            override_count = sum(len(v) for v in overrides.values() if isinstance(v, list))
            if override_count != len(params):
                response["warning"] = f"Expected {len(params)} overrides, found {override_count}"
            response["verification"] = {
                "verified": override_count == len(params),
                "override_count": override_count,
                "expected_count": len(params),
            }
        except Exception as exc:
            logger.warning("Instance verification readback failed: %s", exc, exc_info=True)
            response["verification"] = {
                "verified": None,
                "error_code": "READBACK_FAILED",
                "error": f"Verification readback failed: {exc}",
            }

        return json.dumps(response, indent=2)
