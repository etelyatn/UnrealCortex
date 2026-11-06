"""Explicit registration for the profile-aware operation schema tool."""

from __future__ import annotations

from cortex_mcp.operation_schema import build_profile_operation_schema


def register_profile_standalone_tools(mcp, connection) -> None:
    @mcp.tool(
        name="profile_operation_schema",
        description=(
            "Return the operation contract for one profile/domain/command: live-schema availability, "
            "the selected profile policy, the canonical execution shape and the remaining correction "
            "budget. A failed live lookup retains the complete Unreal error triplet under "
            "`unreal_error`. Requires a running editor for a live schema.\n"
        ),
    )
    def profile_operation_schema(profile: str, domain: str, command: str) -> str:
        return build_profile_operation_schema(connection, profile, domain, command)
