"""MCP composite tools for level scene operations."""

import json
import logging

from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

_VALID_OPS = {"spawn", "modify", "delete", "duplicate", "attach", "detach"}


def _check_ref(ref, defined_ids, op_index):
    """Validate a $ops[id] reference points to a previously defined id.

    Plain labels (non-$ops) pass through without validation.
    Raises ValueError for malformed refs, forward references, or unsupported accessors.
    """
    if not (isinstance(ref, str) and ref.startswith("$ops[")):
        return
    try:
        bracket_pos = ref.index("]")
        ref_id = ref[len("$ops[") : bracket_pos]
    except ValueError as exc:
        raise ValueError(f"Operation at index {op_index} has malformed ref: '{ref}'") from exc
    suffix = ref[bracket_pos + 1 :]
    if suffix != ".name":
        raise ValueError(
            f"Operation at index {op_index} uses unsupported ref accessor '{suffix}'. "
            f"Only '$ops[{ref_id}].name' is supported."
        )
    if ref_id not in defined_ids:
        raise ValueError(
            f"Operation at index {op_index} has forward or unknown ref: '{ref_id}'. "
            f"Ids defined before this op: {sorted(defined_ids)}"
        )


def _validate_level_batch_spec(operations):
    """Validate level batch spec. Raises ValueError on invalid spec."""
    if not isinstance(operations, list):
        raise ValueError("operations must be an array")

    # Collect all ids in one pass to detect duplicates.
    all_ids = []
    for op in operations:
        op_id = op.get("id")
        if op_id:
            all_ids.append(op_id)
    if len(all_ids) != len(set(all_ids)):
        seen = set()
        for op_id in all_ids:
            if op_id in seen:
                raise ValueError(f"Duplicate id: '{op_id}'")
            seen.add(op_id)

    # Validate each op and check ref integrity (ids defined so far).
    defined_ids = set()
    for i, op in enumerate(operations):
        if "op" not in op:
            raise ValueError(f"Operation at index {i} missing 'op' field")
        op_type = op["op"]
        if op_type not in _VALID_OPS:
            raise ValueError(f"Operation at index {i} has unknown op type: '{op_type}'")

        # Validate actor field is non-empty where required.
        if op_type != "spawn" and "actor" in op and not op["actor"]:
            raise ValueError(f"Operation at index {i} has empty 'actor' field")

        if op_type == "spawn":
            if "class" not in op:
                raise ValueError(f"Spawn operation at index {i} missing 'class' field")
        elif op_type == "modify":
            if "actor" not in op:
                raise ValueError(f"Modify operation at index {i} missing 'actor' field")
            _check_ref(op["actor"], defined_ids, i)
            mod_fields = {"label", "folder", "tags", "data_layer", "transform", "properties"}
            if not any(field in op for field in mod_fields):
                raise ValueError(f"Modify operation at index {i} has no modification fields")
        elif op_type == "delete":
            if "actor" not in op:
                raise ValueError(f"Delete operation at index {i} missing 'actor' field")
            _check_ref(op["actor"], defined_ids, i)
        elif op_type == "duplicate":
            if "actor" not in op:
                raise ValueError(f"Duplicate operation at index {i} missing 'actor' field")
            _check_ref(op["actor"], defined_ids, i)
        elif op_type == "attach":
            if "actor" not in op:
                raise ValueError(f"Attach operation at index {i} missing 'actor' field")
            if "parent" not in op:
                raise ValueError(f"Attach operation at index {i} missing 'parent' field")
            _check_ref(op["actor"], defined_ids, i)
            _check_ref(op["parent"], defined_ids, i)
        elif op_type == "detach":
            if "actor" not in op:
                raise ValueError(f"Detach operation at index {i} missing 'actor' field")
            _check_ref(op["actor"], defined_ids, i)

        # Guard against $steps[ injection in property values.
        for prop_path, value in (op.get("properties") or {}).items():
            if isinstance(value, str) and "$steps[" in value:
                raise ValueError(
                    f"Operation '{op.get('id', i)}' property '{prop_path}' contains "
                    f"'$steps[' which conflicts with batch $ref syntax"
                )

        # Register id after validation so forward refs are caught correctly.
        op_id = op.get("id")
        if op_id:
            defined_ids.add(op_id)


def _resolve_ref(ref, id_to_step):
    """Resolve $ops[id].name to $steps[N].data.name, or return ref unchanged."""
    if isinstance(ref, str) and ref.startswith("$ops["):
        ref_id = ref[len("$ops[") : ref.index("]")]
        step = id_to_step[ref_id]
        return f"$steps[{step}].data.name"
    return ref


def _append_property_command(commands, actor_ref, prop_path, value):
    """Append set_component_property or set_actor_property command."""
    parts = prop_path.split(".", 1)
    if len(parts) == 2:
        commands.append(
            {
                "command": "level.set_component_property",
                "params": {
                    "actor": actor_ref,
                    "component": parts[0],
                    "property": parts[1],
                    "value": value,
                },
            }
        )
    else:
        commands.append(
            {
                "command": "level.set_actor_property",
                "params": {"actor": actor_ref, "property": prop_path, "value": value},
            }
        )


def _build_level_batch_commands(operations, save):
    """Translate level batch spec into TCP batch commands.

    Returns:
        commands: list of TCP command dicts
        id_to_step: maps id alias -> step index of the producing command
        step_to_op_info: maps first step index of each op -> {op_index, op_id}
    """
    commands = []
    id_to_step = {}
    step_to_op_info = {}

    for op_index, op in enumerate(operations):
        op_type = op["op"]
        op_id = op.get("id")
        actor_label = op.get("actor") or op.get("label")
        first_step = len(commands)
        step_to_op_info[first_step] = {
            "op_index": op_index,
            "op_id": op_id,
            "actor": actor_label,
        }

        if op_type == "spawn":
            spawn_params = {"class": op["class"]}
            for field in ("location", "rotation", "scale", "label", "mesh", "material"):
                if field in op:
                    spawn_params[field] = op[field]
            commands.append({"command": "level.spawn_actor", "params": spawn_params})

            if op_id:
                id_to_step[op_id] = first_step

            actor_ref = f"$steps[{first_step}].data.name"
            if "folder" in op:
                commands.append(
                    {
                        "command": "level.set_folder",
                        "params": {"actor": actor_ref, "folder": op["folder"]},
                    }
                )
            if "tags" in op:
                commands.append(
                    {
                        "command": "level.set_tags",
                        "params": {"actor": actor_ref, "tags": op["tags"]},
                    }
                )
            for prop_path, value in (op.get("properties") or {}).items():
                _append_property_command(commands, actor_ref, prop_path, value)

        elif op_type == "modify":
            actor_ref = _resolve_ref(op["actor"], id_to_step)
            if "transform" in op:
                commands.append(
                    {
                        "command": "level.set_transform",
                        "params": {"actor": actor_ref, **op["transform"]},
                    }
                )
            if "label" in op:
                commands.append(
                    {
                        "command": "level.rename_actor",
                        "params": {"actor": actor_ref, "label": op["label"]},
                    }
                )
            if "folder" in op:
                commands.append(
                    {
                        "command": "level.set_folder",
                        "params": {"actor": actor_ref, "folder": op["folder"]},
                    }
                )
            if "tags" in op:
                commands.append(
                    {
                        "command": "level.set_tags",
                        "params": {"actor": actor_ref, "tags": op["tags"]},
                    }
                )
            if "data_layer" in op:
                commands.append(
                    {
                        "command": "level.set_data_layer",
                        "params": {"actors": [actor_ref], "data_layer": op["data_layer"]},
                    }
                )
            for prop_path, value in (op.get("properties") or {}).items():
                _append_property_command(commands, actor_ref, prop_path, value)

        elif op_type == "delete":
            actor_ref = _resolve_ref(op["actor"], id_to_step)
            params = {"actor": actor_ref}
            if "confirm_class" in op:
                params["confirm_class"] = op["confirm_class"]
            commands.append({"command": "level.delete_actor", "params": params})

        elif op_type == "duplicate":
            actor_ref = _resolve_ref(op["actor"], id_to_step)
            params = {"actor": actor_ref}
            if "offset" in op:
                params["offset"] = op["offset"]
            commands.append({"command": "level.duplicate_actor", "params": params})
            if op_id:
                id_to_step[op_id] = first_step

        elif op_type == "attach":
            actor_ref = _resolve_ref(op["actor"], id_to_step)
            parent_ref = _resolve_ref(op["parent"], id_to_step)
            params = {"actor": actor_ref, "parent": parent_ref}
            if "socket" in op:
                params["socket"] = op["socket"]
            commands.append({"command": "level.attach_actor", "params": params})

        elif op_type == "detach":
            actor_ref = _resolve_ref(op["actor"], id_to_step)
            commands.append({"command": "level.detach_actor", "params": {"actor": actor_ref}})

    if save:
        commands.append({"command": "level.save_level", "params": {}})

    return commands, id_to_step, step_to_op_info


def register_level_composite_tools(mcp, connection: UEConnection):
    """Register level composite MCP tools."""

    @mcp.tool()
    def level_batch(
        operations: list[dict],
        stop_on_error: bool = False,
        save: bool = True,
    ) -> str:
        """Execute a batch of level operations in a single round-trip.

        Use this tool for any combination of spawning, modifying, deleting, duplicating,
        attaching, or detaching actors. Prefer this over individual tools whenever the
        request touches 2+ spawns or 3+ existing actors.

        Operations array (each item has an "op" field):

          spawn   - create a new actor
            Required: class (str)
            Optional: id, label, location, rotation, scale, mesh, material,
                      folder, tags, properties

          modify  - update an existing actor
            Required: actor (label or $ops[id].name)
            Optional: label, folder, tags, data_layer,
                      transform {location?, rotation?, scale?}, properties

          delete  - remove an actor
            Required: actor
            Optional: confirm_class (safety check, e.g. "StaticMeshActor")

          duplicate - clone an actor
            Required: actor
            Optional: id, offset [X,Y,Z]

          attach  - parent one actor to another
            Required: actor, parent
            Optional: socket

          detach  - unparent an actor
            Required: actor

        Cross-batch references: use "$ops[id].name" to reference an actor spawned
        or duplicated earlier in the same batch. Only actors with an "id" field can be
        referenced. Pre-existing actors are referenced by their label directly.

        Example (build and attach):
            operations=[
                {"op": "spawn", "id": "body", "class": "StaticMeshActor", "label": "Hull"},
                {"op": "spawn", "id": "light", "class": "PointLight",
                 "properties": {"PointLightComponent0.Intensity": 5000}},
                {"op": "attach", "actor": "$ops[light].name", "parent": "$ops[body].name"},
                {"op": "modify", "actor": "OldLight", "folder": "Lighting/Deprecated"},
            ]

        stop_on_error: set True when operations reference each other via $ops[] refs
                       (a failed spawn will make dependent attach/modify ops fail anyway).
                       Leave False for independent bulk modifications.

        save: call save_level at end of batch (default True).
        """
        try:
            _validate_level_batch_spec(operations)
        except ValueError as exc:
            return json.dumps({"success": False, "error": f"Invalid spec: {exc}"})

        commands, _id_to_step, step_to_op_info = _build_level_batch_commands(operations, save)
        total_steps = len(commands)

        max_batch_size = 200
        if total_steps > max_batch_size:
            return json.dumps(
                {
                    "success": False,
                    "error": (
                        f"Batch expands to {total_steps} commands, exceeding limit of "
                        f"{max_batch_size}. Split into smaller batches."
                    ),
                }
            )

        timeout = max(60, len(commands) * 2)
        try:
            batch_result = connection.send_command(
                "batch",
                {
                    "stop_on_error": stop_on_error,
                    "commands": commands,
                },
                timeout=timeout,
            )
        except RuntimeError as exc:
            return json.dumps({"success": False, "error": str(exc)})
        except (ConnectionError, TimeoutError, OSError) as exc:
            return json.dumps({"success": False, "error": f"Connection error: {exc}"})

        batch_data = batch_result.get("data", {})
        results = batch_data.get("results", [])

        completed_count = 0
        spawned_actors = []
        failed_steps = []

        for entry in results:
            if entry.get("success"):
                completed_count += 1
                if entry.get("command") in ("level.spawn_actor", "level.duplicate_actor"):
                    name = (entry.get("data") or {}).get("name", "")
                    if name:
                        spawned_actors.append(name)
            else:
                step_index = entry.get("index", -1)
                # Map step index back to the op that owns it.
                op_info = step_to_op_info.get(step_index)
                if op_info is None:
                    for start_index in sorted(step_to_op_info.keys(), reverse=True):
                        if start_index <= step_index:
                            op_info = step_to_op_info[start_index]
                            break
                op_id = op_info.get("op_id") if op_info else None
                actor = op_info.get("actor") if op_info else None
                failed_steps.append(
                    {
                        "index": step_index,
                        "op_id": op_id or actor,
                        "command": entry.get("command", ""),
                        "error_code": entry.get("error_code", ""),
                        "error": entry.get("error_message", "Unknown error"),
                    }
                )

        if failed_steps:
            return json.dumps(
                {
                    "success": False,
                    "completed_steps": completed_count,
                    "total_steps": total_steps,
                    "spawned_actors": spawned_actors,
                    "failed_steps": failed_steps,
                },
                indent=2,
            )

        return json.dumps(
            {
                "success": True,
                "actor_count": len(spawned_actors),
                "spawned_actors": spawned_actors,
                "total_steps": total_steps,
                "completed_steps": completed_count,
                "total_timing_ms": batch_data.get("total_timing_ms", 0),
            },
            indent=2,
        )
