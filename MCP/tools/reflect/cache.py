"""MCP tools for reflect cache management."""

import json
import logging
import os
import pathlib
import time
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_CACHE_DIR_NAME = "CortexReflect"


def _get_cache_dir() -> pathlib.Path:
    """Get the cache directory path (Saved/CortexReflect/).

    Uses CORTEX_PROJECT_DIR env var if set, otherwise walks up to find .uproject.
    """
    project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    if project_dir:
        return pathlib.Path(project_dir) / "Saved" / _CACHE_DIR_NAME

    current = pathlib.Path(__file__).resolve().parent
    for _ in range(20):
        if list(current.glob("*.uproject")):
            return current / "Saved" / _CACHE_DIR_NAME
        parent = current.parent
        if parent == current:
            break
        current = parent

    # Fallback: 6 levels up from MCP/tools/reflect/cache.py -> project root
    return (
        pathlib.Path(__file__).parent.parent.parent.parent.parent.parent
        / "Saved"
        / _CACHE_DIR_NAME
    )


def register_reflect_cache_tools(mcp, connection: UEConnection):
    """Register cache management tools."""

    @mcp.tool()
    def scan_project(root: str = "AActor") -> str:
        """Full project scan — builds cache of class hierarchy and details.

        Call this once at session start to populate the cache.
        Subsequent queries will be served from cache (instant).

        Args:
            root: Root class to scan from (default 'AActor'). Use 'UObject' for everything.

        Returns:
            Scan summary with class count and timing.
        """
        try:
            start = time.time()
            response = connection.send_command_cached(
                "reflect.class_hierarchy",
                {
                    "root": root,
                    "depth": 10,
                    "max_results": 5000,
                    "include_engine": False,
                },
                ttl=3600,
            )
            elapsed = time.time() - start
            data = response.get("data", {})

            # Write meta.json so reflect_cache_status can check freshness
            cache_dir = _get_cache_dir()
            cache_dir.mkdir(parents=True, exist_ok=True)
            meta = {
                "timestamp": time.time(),
                "class_count": data.get("total_classes", 0),
                "cache_mode": "persistent",
                "root": root,
            }
            (cache_dir / "meta.json").write_text(json.dumps(meta))

            return format_response(
                {
                    "status": "complete",
                    "total_classes": data.get("total_classes", 0),
                    "cpp_count": data.get("cpp_count", 0),
                    "blueprint_count": data.get("blueprint_count", 0),
                    "scan_time_seconds": round(elapsed, 2),
                },
                "scan_project",
            )
        except ConnectionError as e:
            return f"Error: {e}"

    @mcp.tool()
    def reflect_cache_status() -> str:
        """Use at session start to check if the project knowledge graph is cached and fresh.

        Returns cache age, entry count, and whether it needs refreshing.
        Fast call with no side effects.

        Returns:
            Cache metadata: cached (bool), age_seconds, class_count, stale flag.
        """
        cache_dir = _get_cache_dir()
        meta_file = cache_dir / "meta.json"

        if not meta_file.exists():
            return json.dumps(
                {"cached": False, "suggestion": "Run scan_project to build the cache."},
                indent=2,
            )

        try:
            meta = json.loads(meta_file.read_text())
            age = time.time() - meta.get("timestamp", 0)
            return json.dumps(
                {
                    "cached": True,
                    "age_seconds": int(age),
                    "class_count": meta.get("class_count", 0),
                    "cache_mode": meta.get("cache_mode", "persistent"),
                    "stale": age > 3600,
                },
                indent=2,
            )
        except (json.JSONDecodeError, OSError) as e:
            return json.dumps({"cached": False, "error": str(e)}, indent=2)

    @mcp.tool()
    def rebuild_graph_cache() -> str:
        """Force full refresh of the project knowledge graph cache.

        Clears all cached data and re-scans the project.
        Use after major changes like adding new C++ classes or bulk Blueprint operations.

        Returns:
            Rebuild summary with timing.
        """
        # Clear in-memory TCP cache for reflect commands
        connection.invalidate_cache("reflect.")

        # Clear file cache
        cache_dir = _get_cache_dir()
        if cache_dir.exists():
            import shutil

            shutil.rmtree(cache_dir, ignore_errors=True)

        # Re-scan
        return scan_project()
