"""MCP composite tools for high-level level scene construction."""

import json
import logging
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def _validate_scene_spec(actors, organization):
    """Validate the scene spec. Raises ValueError on invalid spec."""
    if not isinstance(actors, list):
        raise ValueError("actors must be an array")

    ids = []
    for i, actor in enumerate(actors):
        if "class" not in actor:
            raise ValueError(f"Actor at index {i} missing 'class' field")
        actor_id = actor.get("id", f"actor_{i}")
        actor["id"] = actor_id
        ids.append(actor_id)

    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate actor IDs in spec")

    for actor in actors:
        for prop_path, value in (actor.get("properties") or {}).items():
            if isinstance(value, str) and "$steps[" in value:
                raise ValueError(
                    f"Actor '{actor.get('id')}' property '{prop_path}' contains '$steps[' "
                    f"which conflicts with batch $ref syntax"
                )

    if organization:
        for att in organization.get("attachments", []):
            if "child" not in att or "parent" not in att:
                raise ValueError("Attachment missing 'child' or 'parent'")
            if att["child"] not in ids:
                raise ValueError(f"Unknown attachment child: {att['child']}")
            if att["parent"] not in ids:
                raise ValueError(f"Unknown attachment parent: {att['parent']}")


def _build_batch_commands(actors, organization, save):
    """Translate scene spec into batch commands with $ref wiring."""
    commands = []
    id_to_step = {}

    for actor in actors:
        actor_id = actor["id"]
        step_index = len(commands)
        id_to_step[actor_id] = step_index

        spawn_params = {"class": actor["class"]}
        if "location" in actor:
            spawn_params["location"] = actor["location"]
        if "rotation" in actor:
            spawn_params["rotation"] = actor["rotation"]
        if "scale" in actor:
            spawn_params["scale"] = actor["scale"]
        if "label" in actor:
            spawn_params["label"] = actor["label"]
        if "mesh" in actor:
            spawn_params["mesh"] = actor["mesh"]
        if "material" in actor:
            spawn_params["material"] = actor["material"]

        commands.append({
            "command": "level.spawn_actor",
            "params": spawn_params,
        })

        if "folder" in actor:
            commands.append({
                "command": "level.set_folder",
                "params": {
                    "actor": f"$steps[{step_index}].data.name",
                    "folder": actor["folder"],
                },
            })

        if "tags" in actor:
            commands.append({
                "command": "level.set_tags",
                "params": {
                    "actor": f"$steps[{step_index}].data.name",
                    "tags": actor["tags"],
                },
            })

        for prop_path, value in (actor.get("properties") or {}).items():
            parts = prop_path.split(".", 1)
            if len(parts) == 2:
                commands.append({
                    "command": "level.set_component_property",
                    "params": {
                        "actor": f"$steps[{step_index}].data.name",
                        "component": parts[0],
                        "property": parts[1],
                        "value": value,
                    },
                })
            else:
                commands.append({
                    "command": "level.set_actor_property",
                    "params": {
                        "actor": f"$steps[{step_index}].data.name",
                        "property": prop_path,
                        "value": value,
                    },
                })

    if organization:
        for att in organization.get("attachments", []):
            child_step = id_to_step[att["child"]]
            parent_step = id_to_step[att["parent"]]
            params = {
                "actor": f"$steps[{child_step}].data.name",
                "parent": f"$steps[{parent_step}].data.name",
            }
            if "socket" in att:
                params["socket"] = att["socket"]
            commands.append({
                "command": "level.attach_actor",
                "params": params,
            })

    if save:
        commands.append({
            "command": "level.save_level",
            "params": {},
        })

    return commands


def register_level_composite_tools(mcp, connection: UEConnection):
    """Register level composite MCP tools."""

    @mcp.tool()
    def create_level_scene(
        actors: list[dict],
        organization: dict | None = None,
        save: bool = True,
    ) -> str:
        """Create a level scene with multiple actors in a single batch operation."""
        try:
            _validate_scene_spec(actors, organization)
        except ValueError as e:
            return json.dumps({"success": False, "error": f"Invalid spec: {e}"})

        commands = _build_batch_commands(actors, organization, save)
        total_steps = len(commands)

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

        failed_step = None
        completed_count = 0
        spawned_actors = []

        for entry in results:
            if entry.get("success"):
                completed_count += 1
                if entry.get("command") == "level.spawn_actor" and "data" in entry:
                    spawned_actors.append(entry["data"].get("name", ""))
            else:
                failed_step = entry
                break

        if failed_step is not None:
            return json.dumps({
                "success": False,
                "summary": f"Step {failed_step['index']} of {total_steps} failed: "
                           f"{failed_step.get('command', '?')} - "
                           f"{failed_step.get('error_message', 'Unknown error')}",
                "completed_steps": completed_count,
                "failed_step": {
                    "index": failed_step["index"],
                    "command": failed_step.get("command", ""),
                    "error": failed_step.get("error_message", ""),
                },
                "total_steps": total_steps,
                "spawned_actors": spawned_actors,
            }, indent=2)

        response = {
            "success": True,
            "actor_count": len(spawned_actors),
            "spawned_actors": spawned_actors,
            "total_steps": total_steps,
            "completed_steps": completed_count,
            "total_timing_ms": batch_data.get("total_timing_ms", 0),
        }

        return json.dumps(response, indent=2)
