"""Capabilities cache loading and router docstring generation."""

from __future__ import annotations

import json
import logging
from pathlib import Path

from .tcp_client import _find_saved_dir


logger = logging.getLogger(__name__)

_DOMAINS = (
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


_COMPOSITE_HINTS: dict[str, str] = {
    "material": "\nFor creating a full material graph from scratch, use material_compose instead of chaining material_cmd calls.",
    "blueprint": "\nFor creating or updating a full Blueprint, use blueprint_compose instead of chaining blueprint_cmd calls.",
    "umg": "\nFor creating a complete Widget Blueprint screen, use widget_compose instead of chaining umg_cmd calls.",
    "level": "\nFor batch actor operations, use level_compose instead of chaining level_cmd calls.",
}


def minimal_router_docstrings() -> dict[str, str]:
    """Return minimal router docstrings when no capabilities cache is available."""
    docstrings: dict[str, str] = {}
    for domain in _DOMAINS:
        tool_name = f"{domain}_cmd"
        docstrings[domain] = (
            f"Route UnrealCortex {domain} commands through `{tool_name}(command, params)`."
            + _COMPOSITE_HINTS.get(domain, "")
        )

    docstrings["core"] += "\nAvailable commands:\n- get_status()\n- save_asset(asset_path: string, only_if_is_dirty: boolean = optional)"
    docstrings["data"] += "\nAvailable commands:\n- query_datatable(table_path: string, row_filter: string = optional)"
    return docstrings


def build_router_docstrings(capabilities: dict | None) -> dict[str, str]:
    """Build per-domain router docstrings from cached capabilities."""
    if capabilities is None:
        return minimal_router_docstrings()

    domains = capabilities.get("domains")
    if not isinstance(domains, dict):
        logger.warning("Capabilities cache has unexpected shape; using minimal router docstrings")
        return minimal_router_docstrings()

    docstrings = minimal_router_docstrings()
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
        docstrings[domain_name] = "\n".join(lines) + _COMPOSITE_HINTS.get(domain_name, "")

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
