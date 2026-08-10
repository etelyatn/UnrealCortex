"""Capabilities cache loading and router docstring generation."""

from __future__ import annotations

import json
import logging
from pathlib import Path

from ._fallback_generated import FALLBACK_COMMANDS as _FALLBACK_STRUCTURED
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
    "statetree",
)

_OPTIONAL_DOMAINS = ("gen", "anim")


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


_missing = set(CORE_DOMAINS) - set(_FALLBACK_STRUCTURED)
if _missing:
    raise ImportError(
        f"Generated fallback missing core domains: {_missing}. "
        f"Run: cd MCP && uv run python scripts/sync_fallback.py"
    )


_COMPOSITE_HINTS: dict[str, str] = {
    "data": (
        "For large raw DataTable, StringTable, or DataAsset reads, use "
        "export_datatable_json, export_string_table_json, export_data_assets_json, "
        "export_schema_json, export_bulk_json, or compare_data_json through data_cmd. "
        "Schema snapshots and raw exports write JSON files and return compact summaries; "
        "do not read exported files back into MCP responses.\n"
    ),
    "editor": (
        "run_python is a high-trust escape hatch for trusted local editor automation; "
        "prefer structured Cortex commands first because run_python can mutate assets/files "
        "and bypass normal safeguards.\n"
    ),
    "material": "For creating a full material graph from scratch, use material_compose instead of chaining material_cmd calls.\n",
    "blueprint": "For creating or updating a full Blueprint, use blueprint_compose instead of chaining blueprint_cmd calls.\n",
    "umg": "For creating a complete Widget Blueprint screen, use widget_compose instead of chaining umg_cmd calls.\n",
    "level": "For batch actor operations, use level_compose instead of chaining level_cmd calls.\n",
    "statetree": "For creating or updating a full StateTree structure, use statetree_compose instead of chaining statetree_cmd calls.\n",
    "gen": "AI asset generation. Submit with start_mesh/start_image/start_texturing, then poll with job_status until status is 'imported' or 'failed'. Generation takes 30-180 seconds. On download_failed or import_failed, call retry_import.\n",
}

_ANIM_INSPECTION_COMMANDS = {
    "list_assets",
    "get_sequence_info",
    "get_montage_info",
    "get_skeleton_info",
    "get_animbp_info",
}
_ANIM_AUTHORING_FAMILIES = {
    "named notify": {"add_named_notify", "update_named_notify", "remove_named_notify"},
    "float curve": {"add_curve", "set_curve_keys", "remove_curve"},
    "montage section": {"add_montage_section", "update_montage_section", "remove_montage_section"},
    "skeleton socket": {"add_socket", "set_socket_transform", "remove_socket"},
    "object notify": {"add_notify", "update_notify", "remove_notify"},
    "notify state": {"add_notify_state", "update_notify_state", "remove_notify_state"},
}


def _build_animation_hint(command_names: set[str]) -> str:
    """Build animation guidance from complete live command families only."""
    lines = ["CortexAnimation supports inspection plus guarded authoring proven by live capabilities."]
    complete_families = [
        family_name
        for family_name, family_commands in _ANIM_AUTHORING_FAMILIES.items()
        if family_commands <= command_names
    ]
    if complete_families:
        lines.append("Available guarded authoring families: " + ", ".join(complete_families) + ".")
        if "named notify" in complete_families:
            lines.append("Phase B sequence named-notify authoring is available.")
    lines.append(
        "Use precise canonical selectors for updates/removes, including montage selector { index, name, start_time } "
        "and socket selector { index, socket_name, bone_name }, pass expected_fingerprint for mutations, use "
        "dry_run=true to preview, and remember save defaults to false."
    )
    lines.append(
        "Only call animation authoring commands that appear in live capabilities and belong to a complete family. "
        "Do not invent later-stage AnimBP authoring, blendspace, retargeting, runtime preview, Sequencer, or Control Rig commands.\n"
    )
    return " ".join(lines)


def _animation_commands_for_docstring(commands: list[dict]) -> list[dict]:
    """Keep inspection commands and only complete live animation authoring families."""
    command_names = {command.get("name") for command in commands}
    allowed = set(_ANIM_INSPECTION_COMMANDS)
    for family_commands in _ANIM_AUTHORING_FAMILIES.values():
        if family_commands <= command_names:
            allowed.update(family_commands)
    return [command for command in commands if command.get("name") in allowed]


def _composite_hint(domain: str, command_names: set[str] | None = None) -> str:
    if domain == "anim":
        return _build_animation_hint(command_names or set())
    return _COMPOSITE_HINTS.get(domain, "")


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
        hint = _composite_hint(domain)
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
        command_names = {command.get("name") for command in commands}
        if domain_name == "anim":
            commands = _animation_commands_for_docstring(commands)
        lines = [
            f"Route UnrealCortex {domain_name} commands through `{domain_name}_cmd(command, params)`.",
            "Available commands:",
        ]
        for command in commands:
            lines.append(f"- {_format_command_signature(command)}")
        hint = _composite_hint(domain_name, command_names)
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


def _build_fallback_strings() -> dict[str, str]:
    """Format structured fallback data into docstring-ready command lists."""
    result: dict[str, str] = {}
    for domain, commands in _FALLBACK_STRUCTURED.items():
        lines = ["\nAvailable commands:"]
        for cmd in commands:
            lines.append(f"\n- {_format_command_signature(cmd)}")
        result[domain] = "".join(lines)
    return result


_FALLBACK_COMMANDS = _build_fallback_strings()
