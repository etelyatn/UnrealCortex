"""Capabilities cache loading and router docstring generation."""

from __future__ import annotations

import json
import logging
from pathlib import Path

from .tcp_client import _find_saved_dir


logger = logging.getLogger(__name__)

CORE_DOMAINS = (
    "core",
    "data",
    "blueprint",
    "graph",
    "level",
    "material",
    "umg",
    "qa",
    "reflect",
    "editor",
)

_OPTIONAL_DOMAINS = ("gen",)


def get_registered_domains(capabilities: dict | None = None) -> tuple[str, ...]:
    """Return core domains + any optional domains found in capabilities cache.

    Optional domains (e.g., gen) are only included when the editor has them
    registered.  No cache = core domains only (safe default).
    """
    if capabilities is None:
        return CORE_DOMAINS

    domains_data = capabilities.get("domains")
    if not isinstance(domains_data, dict):
        return CORE_DOMAINS

    extra = tuple(d for d in _OPTIONAL_DOMAINS if d in domains_data)
    return CORE_DOMAINS + extra


def load_capabilities_cache() -> dict | None:
    """Load the persisted capabilities cache from Saved/Cortex/ if present."""
    saved_dir = _find_saved_dir()
    if saved_dir is None:
        logger.warning("Saved directory not found; capabilities cache unavailable")
        return None

    cache_path = saved_dir / "Cortex" / "capabilities-cache.json"
    if not cache_path.is_file():
        logger.warning("Capabilities cache not found at %s", cache_path)
        return None

    try:
        return json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        logger.warning("Failed to load capabilities cache from %s: %s", cache_path, exc)
        return None


_FALLBACK_COMMANDS: dict[str, str] = {
    "core": (
        "\nAvailable commands:"
        "\n- get_status()"
        "\n- save_asset(asset_path: string, only_if_is_dirty: boolean = optional)"
        "\n- open_asset(asset_path: string, dry_run: boolean = optional)"
        "\n- close_asset(asset_path: string, save: boolean = optional, dry_run: boolean = optional)"
        "\n- reload_asset(asset_path: string, dry_run: boolean = optional)"
        "\n- delete_asset(asset_path: string)"
        "\n- delete_folder(folder_path: string)"
        "\n- shutdown()"
        "\n- switch_editor(pid: number = optional)"
        "\n- schema_status()"
        "\n- get_data_catalog()"
        "\n- batch_query(queries: array)"
    ),
    "data": (
        "\nAvailable commands:"
        "\n- create_datatable(name: string, path: string, row_struct: string)"
        "\n- list_datatables(path: string = optional)"
        "\n- get_datatable_schema(table_path: string)"
        "\n- query_datatable(table_path: string, row_filter: string = optional)"
        "\n- get_datatable_row(table_path: string, row_name: string)"
        "\n- add_datatable_row(table_path: string, row_name: string, values: object)"
        "\n- update_datatable_row(table_path: string, row_name: string, values: object)"
        "\n- delete_datatable_row(table_path: string, row_name: string)"
        "\n- search_datatable_content(query: string)"
        "\n- import_datatable_json(table_path: string, json_data: string)"
        "\n- get_struct_schema(struct_name: string)"
        "\n- get_data_catalog()"
        "\n- resolve_tags(tags: array)"
        "\n- list_gameplay_tags(filter: string = optional)"
        "\n- validate_gameplay_tag(tag: string)"
        "\n- register_gameplay_tag(tag: string, comment: string = optional)"
        "\n- register_gameplay_tags(tags: array)"
        "\n- list_data_assets(path: string = optional)"
        "\n- get_data_asset(asset_path: string)"
        "\n- update_data_asset(asset_path: string, values: object)"
        "\n- create_data_asset(name: string, path: string, class_name: string)"
        "\n- delete_data_asset(asset_path: string)"
        "\n- list_string_tables(path: string = optional)"
        "\n- get_translations(table_path: string)"
        "\n- set_translation(table_path: string, key: string, text: string)"
        "\n- search_assets(query: string, class_filter: string = optional)"
        "\n- list_curve_tables(path: string = optional)"
        "\n- get_curve_table(table_path: string)"
        "\n- update_curve_table_row(table_path: string, row_name: string, values: object)"
    ),
    "blueprint": (
        "\nAvailable commands:"
        "\n- create(name: string, path: string, type: string = optional, parent_class: string = optional)"
        "\n- list(path: string = optional, recursive: boolean = optional)"
        "\n- get_info(asset_path: string)"
        "\n- delete(asset_path: string)"
        "\n- duplicate(asset_path: string, new_name: string, new_path: string = optional)"
        "\n- compile(asset_path: string)"
        "\n- save(asset_path: string)"
        "\n- rename(asset_path: string, new_name: string)"
        "\n- add_variable(asset_path: string, name: string, type: string, category: string = optional)"
        "\n- remove_variable(asset_path: string, name: string)"
        "\n- add_function(asset_path: string, name: string)"
        "\n- get_class_defaults(asset_path: string)"
        "\n- set_class_defaults(asset_path: string, values: object)"
        "\n- configure_timeline(asset_path: string, component_name: string, settings: object)"
        "\n- set_component_defaults(asset_path: string, component_name: string, properties: object)"
        "\n- analyze_for_migration(asset_path: string)"
        "\n- cleanup_migration(asset_path: string)"
        "\n- remove_scs_component(asset_path: string, component_name: string)"
        "\n- recompile_dependents(asset_path: string)"
        "\n- fixup_redirectors(path: string)"
        "\n- compare_blueprints(asset_path: string, cpp_class: string)"
        "\n- delete_orphaned_nodes(asset_path: string, graph_name: string = optional)"
        "\n- search(query: string, path: string = optional)"
        "\n- reparent(asset_path: string, new_parent: string)"
    ),
    "graph": (
        "\nAvailable commands:"
        "\n- list_graphs(asset_path: string)"
        "\n- list_nodes(asset_path: string, graph_name: string)"
        "\n- get_node(asset_path: string, graph_name: string, node_id: string)"
        "\n- search_nodes(asset_path: string, query: string, graph_name: string = optional)"
        "\n- add_node(asset_path: string, graph_name: string, node_class: string)"
        "\n- remove_node(asset_path: string, graph_name: string, node_id: string)"
        "\n- connect(asset_path: string, source_node: string, source_pin: string, target_node: string, target_pin: string)"
        "\n- disconnect(asset_path: string, node_id: string, pin_name: string)"
        "\n- set_pin_value(asset_path: string, graph_name: string, node_id: string, pin_name: string, value: string)"
        "\n- auto_layout(asset_path: string, graph_name: string = optional, mode: string = optional)"
    ),
    "level": (
        "\nAvailable commands:"
        "\n- spawn_actor(class_name: string, label: string = optional, location: object = optional)"
        "\n- delete_actor(label: string)"
        "\n- duplicate_actor(label: string, new_label: string = optional)"
        "\n- rename_actor(label: string, new_label: string)"
        "\n- get_actor(label: string)"
        "\n- set_transform(label: string, location: object = optional, rotation: object = optional, scale: object = optional)"
        "\n- set_actor_property(label: string, property_name: string, value: any)"
        "\n- get_actor_property(label: string, property_name: string)"
        "\n- list_actors(class_filter: string = optional, folder: string = optional)"
        "\n- find_actors(query: string)"
        "\n- get_bounds(label: string)"
        "\n- select_actors(labels: array)"
        "\n- get_selection()"
        "\n- attach_actor(child: string, parent: string)"
        "\n- detach_actor(label: string)"
        "\n- set_tags(label: string, tags: array)"
        "\n- set_folder(label: string, folder: string)"
        "\n- group_actors(labels: array, group_name: string)"
        "\n- ungroup_actors(group_name: string)"
        "\n- list_components(label: string)"
        "\n- add_component(label: string, class_name: string, component_name: string = optional)"
        "\n- remove_component(label: string, component_name: string)"
        "\n- get_component_property(label: string, component_name: string, property_name: string)"
        "\n- set_component_property(label: string, component_name: string, property_name: string, value: any)"
        "\n- list_actor_classes(filter: string = optional)"
        "\n- list_component_classes(filter: string = optional)"
        "\n- describe_class(class_name: string)"
        "\n- get_info()"
        "\n- list_sublevels()"
        "\n- load_sublevel(level_name: string)"
        "\n- unload_sublevel(level_name: string)"
        "\n- set_sublevel_visibility(level_name: string, visible: boolean)"
        "\n- list_data_layers()"
        "\n- set_data_layer(label: string, layer: string)"
        "\n- save_level()"
        "\n- save_all()"
    ),
    "material": (
        "\nAvailable commands:"
        "\n- list_materials(path: string = optional, recursive: boolean = optional)"
        "\n- get_material(asset_path: string)"
        "\n- create_material(asset_path: string, name: string)"
        "\n- delete_material(asset_path: string)"
        "\n- set_material_property(asset_path: string, property_name: string, value: object)"
        "\n- list_instances(path: string = optional, parent_material: string = optional)"
        "\n- get_instance(asset_path: string)"
        "\n- create_instance(asset_path: string, name: string, parent_material: string)"
        "\n- delete_instance(asset_path: string)"
        "\n- list_parameters(asset_path: string)"
        "\n- get_parameter(asset_path: string, parameter_name: string)"
        "\n- set_parameter(asset_path: string, parameter_name: string, parameter_type: string, value: object)"
        "\n- set_parameters(asset_path: string, parameters: array)"
        "\n- reset_parameter(asset_path: string, parameter_name: string)"
        "\n- list_nodes(asset_path: string)"
        "\n- get_node(asset_path: string, node_id: string)"
        "\n- add_node(asset_path: string, expression_class: string, position: object = optional)"
        "\n- remove_node(asset_path: string, node_id: string)"
        "\n- list_connections(asset_path: string)"
        "\n- connect(asset_path: string, source_node: string, source_output: number, target_node: string, target_input: object)"
        "\n- disconnect(asset_path: string, target_node: string, target_input: object)"
        "\n- auto_layout(asset_path: string)"
        "\n- set_node_property(asset_path: string, node_id: string, property_name: string, value: object)"
        "\n- get_node_pins(asset_path: string, node_id: string)"
        "\n- list_collections(path: string = optional, recursive: boolean = optional)"
        "\n- get_collection(asset_path: string)"
        "\n- create_collection(asset_path: string, name: string)"
        "\n- delete_collection(asset_path: string)"
        "\n- add_collection_parameter(asset_path: string, parameter_name: string, parameter_type: string, default_value: object)"
        "\n- remove_collection_parameter(asset_path: string, parameter_name: string)"
        "\n- set_collection_parameter(asset_path: string, parameter_name: string, value: object)"
        "\n- list_dynamic_instances(actor_path: string)"
        "\n- get_dynamic_instance(actor_path: string, component_name: string = optional, slot_index: number = optional)"
        "\n- create_dynamic_instance(actor_path: string, component_name: string = optional, slot_index: number = optional, source_material: string = optional, parameters: array = optional)"
        "\n- destroy_dynamic_instance(actor_path: string, component_name: string = optional, slot_index: number = optional)"
        "\n- set_dynamic_parameter(actor_path: string, name: string, type: string, value: object, component_name: string = optional, slot_index: number = optional)"
        "\n- get_dynamic_parameter(actor_path: string, name: string, component_name: string = optional, slot_index: number = optional)"
        "\n- list_dynamic_parameters(actor_path: string, component_name: string = optional, slot_index: number = optional)"
        "\n- set_dynamic_parameters(actor_path: string, parameters: array, component_name: string = optional, slot_index: number = optional)"
        "\n- reset_dynamic_parameter(actor_path: string, name: string, component_name: string = optional, slot_index: number = optional)"
    ),
    "umg": (
        "\nAvailable commands:"
        "\n- add_widget(asset_path: string, class_name: string, parent_name: string = optional, name: string = optional)"
        "\n- remove_widget(asset_path: string, widget_name: string)"
        "\n- reparent(asset_path: string, widget_name: string, new_parent: string)"
        "\n- get_tree(asset_path: string)"
        "\n- get_widget(asset_path: string, widget_name: string)"
        "\n- list_widget_classes(filter: string = optional)"
        "\n- duplicate_widget(asset_path: string, widget_name: string, new_name: string = optional)"
        "\n- set_color(asset_path: string, widget_name: string, color: object)"
        "\n- set_text(asset_path: string, widget_name: string, text: string)"
        "\n- set_font(asset_path: string, widget_name: string, size: number = optional, typeface: string = optional)"
        "\n- set_brush(asset_path: string, widget_name: string, texture: string = optional, tint: object = optional)"
        "\n- set_padding(asset_path: string, widget_name: string, padding: object)"
        "\n- set_anchor(asset_path: string, widget_name: string, anchor: object)"
        "\n- set_alignment(asset_path: string, widget_name: string, alignment: object)"
        "\n- set_size(asset_path: string, widget_name: string, width: number = optional, height: number = optional)"
        "\n- set_visibility(asset_path: string, widget_name: string, visibility: string)"
        "\n- set_property(asset_path: string, widget_name: string, property_name: string, value: any)"
        "\n- get_property(asset_path: string, widget_name: string, property_name: string)"
        "\n- get_schema(class_name: string)"
        "\n- create_animation(asset_path: string, animation_name: string, tracks: array)"
        "\n- list_animations(asset_path: string)"
        "\n- remove_animation(asset_path: string, animation_name: string)"
    ),
    "qa": (
        "\nAvailable commands:"
        "\n- observe_state(query: string = optional)"
        "\n- get_actor_state(label: string)"
        "\n- get_player_state()"
        "\n- look_at(target: string)"
        "\n- interact(target: string = optional)"
        "\n- move_to(target: string = optional, location: object = optional)"
        "\n- wait_for(condition: string, timeout: number = optional)"
        "\n- teleport_player(location: object)"
        "\n- set_actor_property(label: string, property_name: string, value: any)"
        "\n- set_random_seed(seed: number)"
        "\n- assert_state(assertions: array)"
        "\n- start_recording(name: string = optional)"
        "\n- stop_recording()"
        "\n- replay_session(session_id: string)"
        "\n- cancel_replay()"
    ),
    "reflect": (
        "\nAvailable commands:"
        "\n- class_hierarchy(class_name: string, depth: number = optional)"
        "\n- class_detail(class_name: string)"
        "\n- find_usages(class_name: string, member_name: string = optional)"
        "\n- find_overrides(class_name: string, function_name: string)"
        "\n- search(query: string)"
        "\n- get_dependencies(asset_path: string)"
        "\n- get_referencers(asset_path: string)"
    ),
    "editor": (
        "\nAvailable commands:"
        "\n- start_pie()"
        "\n- stop_pie()"
        "\n- pause_pie()"
        "\n- resume_pie()"
        "\n- restart_pie()"
        "\n- get_pie_state()"
        "\n- inject_key(key: string, event: string = optional)"
        "\n- inject_mouse(button: string, event: string = optional, position: object = optional)"
        "\n- inject_input_action(action_name: string, value: any = optional)"
        "\n- inject_input_sequence(steps: array)"
        "\n- get_editor_state()"
        "\n- execute_console_command(command: string)"
        "\n- set_time_dilation(value: number)"
        "\n- get_world_info()"
        "\n- get_recent_logs(count: number = optional, filter: string = optional)"
        "\n- get_viewport_info()"
        "\n- capture_screenshot(filename: string = optional)"
        "\n- set_viewport_camera(location: object = optional, rotation: object = optional)"
        "\n- focus_actor(label: string)"
        "\n- focus_node(asset_path: string, graph_name: string, node_id: string)"
        "\n- set_viewport_mode(mode: string)"
    ),
    "gen": (
        "\nAvailable commands:"
        "\n- start_mesh(prompt: string, provider: string = optional, destination: string = optional)"
        "\n- start_image(prompt: string, provider: string = optional)"
        "\n- start_texturing(prompt: string, source_model_path: string, provider: string = optional)"
        "\n- job_status(job_id: string)"
        "\n- list_jobs(status_filter: string = optional)"
        "\n- cancel_job(job_id: string)"
        "\n- retry_import(job_id: string)"
        "\n- list_providers()"
        "\n- delete_job(job_id: string)"
        "\n- get_config()"
    ),
}

_ALL_DOMAINS = CORE_DOMAINS + _OPTIONAL_DOMAINS

assert set(_ALL_DOMAINS) == set(_FALLBACK_COMMANDS), (
    f"_ALL_DOMAINS and _FALLBACK_COMMANDS out of sync: "
    f"missing fallback: {set(_ALL_DOMAINS) - set(_FALLBACK_COMMANDS)}, "
    f"extra fallback: {set(_FALLBACK_COMMANDS) - set(_ALL_DOMAINS)}"
)


_COMPOSITE_HINTS: dict[str, str] = {
    "material": "For creating a full material graph from scratch, use material_compose instead of chaining material_cmd calls.\n",
    "blueprint": "For creating or updating a full Blueprint, use blueprint_compose instead of chaining blueprint_cmd calls.\n",
    "umg": "For creating a complete Widget Blueprint screen, use widget_compose instead of chaining umg_cmd calls.\n",
    "level": "For batch actor operations, use level_compose instead of chaining level_cmd calls.\n",
    "gen": "AI asset generation. Submit with start_mesh/start_image/start_texturing, then poll with job_status until status is 'imported' or 'failed'. Generation takes 30-180 seconds. On download_failed or import_failed, call retry_import.\n",
}


def minimal_router_docstrings(domains: tuple[str, ...] | None = None) -> dict[str, str]:
    """Return minimal router docstrings when no capabilities cache is available.

    Every domain gets a hardcoded command list so LLMs know valid command
    names even when the live capabilities cache is unavailable.
    """
    if domains is None:
        domains = CORE_DOMAINS
    docstrings: dict[str, str] = {}
    for domain in domains:
        tool_name = f"{domain}_cmd"
        hint = _COMPOSITE_HINTS.get(domain, "")
        base = f"Route UnrealCortex {domain} commands through `{tool_name}(command, params)`."
        commands = _FALLBACK_COMMANDS.get(domain, "")
        body = base + commands
        docstrings[domain] = (hint + body) if hint else body

    return docstrings


def build_router_docstrings(capabilities: dict | None) -> dict[str, str]:
    """Build per-domain router docstrings from cached capabilities."""
    registered = get_registered_domains(capabilities)

    if capabilities is None:
        return minimal_router_docstrings(registered)

    domains = capabilities.get("domains")
    if not isinstance(domains, dict):
        logger.warning("Capabilities cache has unexpected shape; using minimal router docstrings")
        return minimal_router_docstrings(registered)

    docstrings = minimal_router_docstrings(registered)
    for domain_name, domain_info in domains.items():
        if domain_name not in docstrings:
            continue

        commands = domain_info.get("commands", [])
        lines = [
            f"Route UnrealCortex {domain_name} commands through `{domain_name}_cmd(command, params)`.",
            "Available commands:",
        ]
        for command in commands:
            lines.append(f"- {_format_command_signature(command)}")
        hint = _COMPOSITE_HINTS.get(domain_name, "")
        body = "\n".join(lines)
        docstrings[domain_name] = (hint + body) if hint else body

    return docstrings


def _format_command_signature(command: dict) -> str:
    """Format a command entry into a compact signature string."""
    name = command.get("name", "unknown")
    params = command.get("params", [])
    if not params:
        return f"{name}()"

    parts = []
    for param in params:
        param_name = param.get("name", "param")
        param_type = param.get("type", "any")
        if param.get("required", False):
            parts.append(f"{param_name}: {param_type}")
        else:
            parts.append(f"{param_name}: {param_type} = optional")
    return f"{name}({', '.join(parts)})"
