"""Profile-aware operation schemas served from the live editor with a bounded retry budget."""

from __future__ import annotations

import json
from collections import defaultdict

from . import capabilities as _capabilities_module
from .tcp_client import UECommandError

RETRY_BUDGET_EXHAUSTED = "RETRY_BUDGET_EXHAUSTED"
INVALID_INVOCATION_SHAPE = "INVALID_INVOCATION_SHAPE"
DEFAULT_PROFILE = "UMGAuthoring"
_STATIC_ERROR_BUDGET = 2

PROFILE_POLICIES: dict[str, dict] = {
    DEFAULT_PROFILE: {
        "allowed_domains": frozenset({"umg", "graph", "core"}),
        "blocked_reason": (
            "UMGAuthoring profile restricts direct authoring to the umg, graph, and core domains; "
            "commands in other domains (e.g. blueprint, data, level) are not permitted"
        ),
    },
}

_correction_counts: dict[tuple[str, str, str], int] = defaultdict(int)


def reset_operation_schema_budget() -> None:
    _correction_counts.clear()


def _record_correction(profile: str, domain: str, command: str) -> int:
    key = (profile, domain, command)
    _correction_counts[key] += 1
    return _correction_counts[key]


def resolve_execution_shape(domain: str, command: str) -> dict:
    if command == "batch_query":
        return {"type": "batch", "tool": "core_cmd"}
    return {"type": "router", "tool": f"{domain}_cmd"}


def _live_schema(connection, domain: str, command: str) -> dict | None:
    """Return the live editor's operation schema or a structured failure dict."""
    try:
        response = connection.send_command(
            "core.get_operation_schema", {"domain": domain, "command": command}
        )
        return response.get("data")
    except UECommandError as exc:
        cache = _capabilities_module.load_capabilities_cache() or {}
        cache_advertised = False
        try:
            cache_advertised = command in {
                cmd.get("name")
                for cmd in cache["domains"][domain].get("commands", [])
            }
        except (KeyError, TypeError):
            cache_advertised = False
        return {
            "editor_available": False,
            "error_code": exc.code,
            "error_message": exc.message,
            "cache_advertised": cache_advertised,
            "restart_or_reload_required": cache_advertised,
            "suggested_next_action": (
                "Restart the Unreal Editor with the rebuilt UnrealCortex plugin, or reload the "
                "plugin, then re-verify with profile_operation_schema."
                if cache_advertised
                else "The connected editor does not register this command; verify the plugin "
                "build before calling it."
            ),
        }


def build_profile_operation_schema(connection, profile: str, domain: str, command: str) -> str:
    profile = profile or DEFAULT_PROFILE
    policy = PROFILE_POLICIES.get(profile)
    if policy is None:
        return json.dumps({
            "_error": "INVALID_FIELD",
            "_message": f"Unknown authoring profile '{profile}'",
            "known_profiles": sorted(PROFILE_POLICIES),
        })

    domain_allowed = domain in policy["allowed_domains"]
    schema = _live_schema(connection, domain, command)
    editor_available = bool(schema and schema.get("editor_available", True) is not False)

    if not domain_allowed or not editor_available:
        corrections = _record_correction(profile, domain, command)
        budget_remaining = max(0, _STATIC_ERROR_BUDGET - corrections)
        if corrections > _STATIC_ERROR_BUDGET:
            return json.dumps({
                "_error": RETRY_BUDGET_EXHAUSTED,
                "_message": (
                    f"{profile}/{domain}.{command} has exhausted its correction budget of "
                    f"{_STATIC_ERROR_BUDGET}; re-check the live capabilities or restart the task "
                    "rather than guessing again."
                ),
                "budget_remaining": 0,
            })
        return json.dumps({
            "source": "live_editor" if editor_available else "facade",
            "profile": profile,
            "domain": domain,
            "command": command,
            "editor_available": editor_available,
            "policy_allowed": False,
            "execution_shape": resolve_execution_shape(domain, command),
            "blocked_reason": (
                policy["blocked_reason"] + f"; '{domain}.{command}' is not permitted"
                if not domain_allowed
                else (schema or {}).get("error_message", "Command unavailable in the connected editor")
            ),
            "restart_or_reload_required": bool((schema or {}).get("restart_or_reload_required")),
            "suggested_next_action": (
                "Use graph.describe_node on the target Blueprint and umg.set_widget_variable to "
                "make the designer widget a variable before referencing it from a graph."
                if not domain_allowed
                else (schema or {}).get("suggested_next_action", "Verify the plugin build and re-check the live capabilities.")
            ),
            "budget_remaining": budget_remaining,
        })

    budget_remaining = max(0, _STATIC_ERROR_BUDGET - _correction_counts[(profile, domain, command)])
    return json.dumps({
        "source": "live_editor",
        "profile": profile,
        "domain": domain,
        "command": command,
        "editor_available": True,
        "policy_allowed": True,
        "execution_shape": resolve_execution_shape(domain, command),
        "params": schema.get("params", []),
        "budget_remaining": budget_remaining,
    })
