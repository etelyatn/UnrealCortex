"""Shared pytest path setup for legacy tool-module imports."""

from __future__ import annotations

import os
import sys
import uuid
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from mcp.server.fastmcp import FastMCP
from cortex_mcp.server import _register_explicit_tools
from cortex_mcp.tcp_client import UEConnection
from tools.level.composites import _build_level_batch_commands, _validate_level_batch_spec


_TESTS_DIR = Path(__file__).resolve().parent
_MCP_ROOT = _TESTS_DIR.parent
_PROJECT_ROOT = _MCP_ROOT.parent.parent.parent
_SRC_DIR = _MCP_ROOT / "src"
_TOOLS_DIR = _SRC_DIR / "tools"
_EDITOR_TOOLS_DIR = _TOOLS_DIR / "editor"

for path in (_SRC_DIR, _TOOLS_DIR, _EDITOR_TOOLS_DIR):
    path_str = str(path)
    if path_str not in sys.path:
        sys.path.insert(0, path_str)


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


def _parse_json_object(value):
    if isinstance(value, str):
        parsed = json.loads(value)
        if isinstance(parsed, dict):
            return parsed
    return value


def _normalize_data_args(command: str, args: dict) -> dict:
    normalized = dict(args)
    if command in {"add_datatable_row", "update_datatable_row"} and "row_data" in normalized:
        normalized["row_data"] = _parse_json_object(normalized["row_data"])
    if command == "batch_query" and isinstance(normalized.get("commands"), str):
        normalized["commands"] = json.loads(normalized["commands"])
    return normalized


def _normalize_level_args(command: str, args: dict) -> dict:
    normalized = dict(args)
    if command in {"spawn_actor", "add_component", "describe_class"}:
        class_name = normalized.pop("class_name", None)
        if class_name and "class" not in normalized:
            normalized["class"] = class_name
    return normalized


def _normalize_graph_args(args: dict) -> dict:
    normalized = dict(args)
    if "params" in normalized:
        normalized["params"] = _parse_json_object(normalized["params"])
    return normalized


def _normalize_editor_args(command: str, args: dict) -> dict:
    normalized = dict(args)
    if command == "set_viewport_camera" and "location" not in normalized:
        xyz = {axis: normalized.pop(axis) for axis in ("x", "y", "z") if axis in normalized}
        if len(xyz) == 3:
            normalized["location"] = xyz
    return normalized


def _normalize_material_args(command: str, args: dict) -> dict:
    normalized = dict(args)
    if command == "set_material_property":
        property_name = normalized.get("property_name")
        value = normalized.get("value")
        if property_name == "MaterialDomain" and value == "PostProcess":
            normalized["value"] = "MD_PostProcess"
        if property_name == "BlendMode" and value == "Opaque":
            normalized["value"] = "BLEND_Opaque"
    return normalized


def _format_level_batch_response(batch_data: dict, total_steps: int) -> dict:
    results = batch_data.get("results", [])
    completed_steps = sum(1 for entry in results if entry.get("success"))
    spawned_actors = []
    failed_steps = []

    for entry in results:
        if entry.get("success"):
            if entry.get("command") in {"level.spawn_actor", "level.duplicate_actor"}:
                name = (entry.get("data") or {}).get("name")
                if name:
                    spawned_actors.append(name)
            continue

        failed_steps.append(
            {
                "index": entry.get("index", -1),
                "op_id": None,
                "command": entry.get("command", ""),
                "error_code": entry.get("error_code", ""),
                "error": entry.get("error_message", "Unknown error"),
            }
        )

    if failed_steps:
        return {
            "success": False,
            "completed_steps": completed_steps,
            "total_steps": total_steps,
            "spawned_actors": spawned_actors,
            "failed_steps": failed_steps,
        }

    return {
        "success": True,
        "actor_count": len(spawned_actors),
        "spawned_actors": spawned_actors,
        "total_steps": total_steps,
        "completed_steps": completed_steps,
        "total_timing_ms": batch_data.get("total_timing_ms", 0),
    }


@pytest.fixture(scope="session")
def tcp_connection():
    old_project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    os.environ["CORTEX_PROJECT_DIR"] = str(_PROJECT_ROOT)
    conn = UEConnection()
    conn.connect()
    if old_project_dir is None:
        os.environ.pop("CORTEX_PROJECT_DIR", None)
    else:
        os.environ["CORTEX_PROJECT_DIR"] = old_project_dir
    try:
        yield conn
    finally:
        conn.disconnect()


@pytest.fixture
def cleanup_assets(tcp_connection):
    created: list[tuple[str, str] | str] = []
    try:
        yield created
    finally:
        for entry in reversed(created):
            if isinstance(entry, tuple):
                asset_type, asset_path = entry
            else:
                asset_type, asset_path = "blueprint", entry
            try:
                if asset_type == "material":
                    tcp_connection.send_command("material.delete_material", {"asset_path": asset_path})
                elif asset_type == "material_instance":
                    tcp_connection.send_command("material.delete_instance", {"asset_path": asset_path})
                else:
                    tcp_connection.send_command("bp.delete", {"asset_path": asset_path})
            except Exception:
                pass


@pytest.fixture
def blueprint_for_test(tcp_connection, cleanup_assets):
    name = _uniq("BP_E2E")
    resp = tcp_connection.send_command(
        "bp.create",
        {"name": name, "path": "/Game/Temp/CortexMCPTest", "type": "Actor"},
    )
    asset_path = resp["data"]["asset_path"]
    cleanup_assets.append(("blueprint", asset_path))
    return asset_path


@pytest.fixture
def widget_bp_for_test(tcp_connection, cleanup_assets):
    name = _uniq("WBP_E2E")
    resp = tcp_connection.send_command(
        "bp.create",
        {"name": name, "path": "/Game/Temp/CortexMCPTest", "type": "Widget"},
    )
    asset_path = resp["data"]["asset_path"]
    cleanup_assets.append(("blueprint", asset_path))
    return asset_path


@pytest.fixture
def cleanup_actors(tcp_connection):
    created: list[str] = []
    try:
        yield created
    finally:
        for actor_name in reversed(created):
            try:
                tcp_connection.send_command("level.delete_actor", {"actor": actor_name})
            except Exception:
                pass


@pytest.fixture
def actors_for_test(tcp_connection, cleanup_actors):
    actors = {}
    specs = {
        "light": {"class": "PointLight", "label": _uniq("CortexE2E_Light")},
        "camera": {"class": "CameraActor", "label": _uniq("CortexE2E_Camera")},
        "mesh": {"class": "StaticMeshActor", "label": _uniq("CortexE2E_Mesh")},
    }

    for key, spec in specs.items():
        resp = tcp_connection.send_command(
            "level.spawn_actor",
            {"class": spec["class"], "label": spec["label"]},
        )
        cleanup_actors.append(spec["label"])
        actors[key] = spec["label"]

    return actors


def _map_tool_call(name: str, args: dict) -> tuple[str, dict]:
    if name in {"core_cmd", "data_cmd", "blueprint_cmd", "graph_cmd", "level_cmd", "material_cmd", "umg_cmd", "qa_cmd", "reflect_cmd", "editor_cmd", "blueprint_compose", "material_compose", "material_instance_compose", "widget_compose", "level_compose", "scenario_compose", "editor_restart", "schema_generate", "qa_test_step"}:
        return name, args

    if name == "level_batch":
        return "level_compose", {"kwargs": args}
    if name == "run_scenario_inline":
        return "scenario_compose", args
    if name == "restart_editor":
        return "editor_restart", args
    if name == "generate_project_schema":
        return "schema_generate", args
    if name == "test_step":
        return "qa_test_step", args

    if name.startswith("graph_"):
        return "graph_cmd", {"command": name.removeprefix("graph_"), "params": _normalize_graph_args(args)}

    blueprint_map = {
        "create_blueprint": "create",
        "list_blueprints": "list",
        "get_blueprint_info": "get_info",
        "delete_blueprint": "delete",
        "duplicate_blueprint": "duplicate",
        "compile_blueprint": "compile",
        "save_blueprint": "save",
        "add_blueprint_variable": "add_variable",
        "remove_blueprint_variable": "remove_variable",
        "add_blueprint_function": "add_function",
        "get_class_defaults": "get_class_defaults",
        "set_class_defaults": "set_class_defaults",
    }
    if name in blueprint_map:
        return "__tcp__", {"command": f"bp.{blueprint_map[name]}", "params": args}

    umg_names = {
        "add_widget", "remove_widget", "reparent", "get_tree", "get_widget", "list_widget_classes",
        "duplicate_widget", "set_color", "set_text", "set_font", "set_brush", "set_padding",
        "set_anchor", "set_alignment", "set_size", "set_visibility", "set_property",
        "get_property", "get_schema", "create_animation", "list_animations", "remove_animation",
    }
    if name in umg_names:
        return "umg_cmd", {"command": name, "params": args}

    data_names = {
        "list_datatables", "get_datatable_schema", "query_datatable", "get_datatable_row",
        "add_datatable_row", "update_datatable_row", "delete_datatable_row", "search_datatable_content",
        "batch_query", "get_struct_schema", "list_gameplay_tags", "validate_gameplay_tag",
        "register_gameplay_tag", "register_gameplay_tags", "resolve_tags", "list_data_assets",
        "get_data_asset", "update_data_asset", "list_curve_tables", "get_curve_table",
        "update_curve_table_row", "list_string_tables", "get_translations", "set_translation",
        "remove_translation", "search_assets",
    }
    if name in data_names:
        return "data_cmd", {"command": name, "params": _normalize_data_args(name, args)}

    level_names = {
        "list_actor_classes", "list_component_classes", "describe_class", "spawn_actor", "delete_actor",
        "duplicate_actor", "rename_actor", "get_actor", "set_transform", "set_actor_property",
        "get_actor_property", "list_components", "add_component", "remove_component",
        "get_component_property", "set_component_property", "list_actors", "find_actors",
        "get_bounds", "select_actors", "get_selection", "set_tags", "set_folder", "attach_actor",
        "detach_actor", "group_actors", "ungroup_actors", "get_info", "list_sublevels",
        "load_sublevel", "unload_sublevel", "set_sublevel_visibility", "list_data_layers",
        "save_level", "save_all",
    }
    if name in level_names:
        return "level_cmd", {"command": name, "params": _normalize_level_args(name, args)}

    material_map = {
        "create_material": "create_material",
        "delete_material": "delete_material",
        "get_material": "get_material",
        "create_instance": "create_instance",
        "delete_instance": "delete_instance",
        "get_instance": "get_instance",
        "set_material_property": "set_material_property",
        "add_material_node": "add_node",
        "get_material_node": "get_node",
        "set_material_node_property": "set_node_property",
        "list_materials": "list_materials",
        "list_instances": "list_instances",
        "list_nodes": "list_nodes",
        "material_auto_layout": "auto_layout",
        "auto_layout": "auto_layout",
    }
    if name in material_map:
        command = material_map[name]
        return "material_cmd", {"command": command, "params": _normalize_material_args(command, args)}

    editor_map = {
        "start_pie": "start_pie",
        "stop_pie": "stop_pie",
        "get_pie_state": "get_pie_state",
        "pause_pie": "pause_pie",
        "resume_pie": "resume_pie",
        "restart_pie": "restart_pie",
        "get_viewport_info": "get_viewport_info",
        "capture_screenshot": "capture_screenshot",
        "set_viewport_camera": "set_viewport_camera",
        "focus_actor": "focus_actor",
        "set_viewport_mode": "set_viewport_mode",
        "get_editor_state": "get_editor_state",
        "get_recent_logs": "get_recent_logs",
        "execute_console_command": "execute_console_command",
        "set_time_dilation": "set_time_dilation",
        "get_world_info": "get_world_info",
        "press_key": "inject_key",
        "run_input_sequence": "inject_input_sequence",
    }
    if name in editor_map:
        command = editor_map[name]
        return "editor_cmd", {"command": command, "params": _normalize_editor_args(command, args)}

    if name in {"get_status", "get_data_catalog", "switch_editor", "schema_status", "refresh_cache"}:
        return "core_cmd", {"command": name, "params": args}

    raise KeyError(f"Unmapped MCP tool '{name}'")


@pytest.fixture
def mcp_client(tcp_connection):
    test_mcp = FastMCP("cortex-test")
    _register_explicit_tools(test_mcp, tcp_connection)

    class _Client:
        async def call_tool(self, name: str, args: dict):
            if name == "batch_query":
                normalized_args = _normalize_data_args(name, args)
                response = tcp_connection.send_command(
                    "batch",
                    {"commands": normalized_args["commands"]},
                )
                return SimpleNamespace(content=[SimpleNamespace(text=json.dumps(response.get("data", {})))])

            if name == "level_batch":
                _validate_level_batch_spec(args["operations"])
                commands, _, _ = _build_level_batch_commands(
                    args["operations"],
                    args.get("save", False),
                )
                response = tcp_connection.send_command(
                    "batch",
                    {
                        "commands": commands,
                        "stop_on_error": args.get("stop_on_error", False),
                    },
                )
                formatted = _format_level_batch_response(response.get("data", {}), len(commands))
                return SimpleNamespace(content=[SimpleNamespace(text=json.dumps(formatted))])

            mapped_name, mapped_args = _map_tool_call(name, args)
            if mapped_name == "__tcp__":
                response = tcp_connection.send_command(mapped_args["command"], mapped_args.get("params"))
                return SimpleNamespace(content=[SimpleNamespace(text=json.dumps(response.get("data", {})))])
            result = await test_mcp.call_tool(mapped_name, mapped_args)
            content = result[0] if isinstance(result, tuple) else result
            if isinstance(content, (list, tuple)):
                blocks = list(content)
            else:
                blocks = [SimpleNamespace(text=json.dumps(content))]
            return SimpleNamespace(content=blocks)

    return _Client()
