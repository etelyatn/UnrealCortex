"""StateTree domain tools for CortexMCP.

PR 1 plumbing — `statetree_cmd` is auto-registered by `register_router_tools`
from the `CORE_DOMAINS` tuple in `cortex_mcp.capabilities`. This package will
host higher-level wrapper tools (e.g. `register_statetree_state_tools`,
`register_statetree_node_tools`) added in PRs 2-4.
"""


def register_statetree_tools(mcp, connection):
    """Register all StateTree domain wrapper tools.

    PR 1 ships only the auto-registered `statetree_cmd` router. Subsequent PRs
    will add per-feature register_*_tools imports here:

        from .assets       import register_statetree_asset_tools
        from .states       import register_statetree_state_tools
        from .transitions  import register_statetree_transition_tools
        from .nodes        import register_statetree_node_tools
        from .bindings     import register_statetree_binding_tools
        from .compile      import register_statetree_compile_tools
        from .composites   import register_statetree_composite_tools
    """
    return None
