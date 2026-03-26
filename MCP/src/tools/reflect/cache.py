"""MCP tools for reflect cache management."""

import json
import logging
import os
import pathlib
import time
from cortex_mcp.project import resolve_saved_dir
from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.response import format_response

logger = logging.getLogger(__name__)

_CACHE_DIR_NAME = "Cortex"


def _get_cache_dir() -> pathlib.Path:
    """Get the cache directory path (Saved/Cortex/).

    Delegates to :func:`cortex_mcp.project.resolve_saved_dir` for consistent
    project directory resolution across all MCP components.
    """
    saved = resolve_saved_dir()
    if saved is not None:
        return saved / _CACHE_DIR_NAME

    # Fallback: 6 levels up from MCP/tools/reflect/cache.py -> project root
    return (
        pathlib.Path(__file__).parent.parent.parent.parent.parent.parent
        / "Saved"
        / _CACHE_DIR_NAME
    )


def register_reflect_cache_tools(mcp, connection: UEConnection):
    """Register cache management tools."""

    @mcp.tool()
    def scan_project(root: str = "AActor", project_only: bool = True) -> str:
        """Full project scan — builds cache of class hierarchy and details.

        Call this once at session start to populate the cache.
        Subsequent queries will be served from cache (instant).

        Args:
            root: Root class to scan from (default 'AActor'). Use 'UObject' for everything.
            project_only: If true, only include project classes. If false, include engine too.

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
                    "include_engine": not project_only,
                },
                ttl=3600,
            )
            elapsed = time.time() - start
            data = response.get("data", {})

            try:
                cache_dir = _get_cache_dir()
                cache_dir.mkdir(parents=True, exist_ok=True)

                cache_envelope = {
                    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "params": {
                        "root": root,
                        "depth": 10,
                        "max_results": 5000,
                        "include_engine": not project_only,
                    },
                    "data": data,
                }
                cache_path = cache_dir / "reflect-cache.json"
                tmp_path = cache_path.with_suffix(".json.tmp")
                tmp_path.write_text(json.dumps(cache_envelope))
                tmp_path.replace(cache_path)

                meta = {
                    "timestamp": time.time(),
                    "class_count": data.get("total_classes", 0),
                    "cache_mode": "persistent",
                    "root": root,
                }
                meta_path = cache_dir / "meta.json"
                meta_tmp = meta_path.with_suffix(".json.tmp")
                meta_tmp.write_text(json.dumps(meta))
                meta_tmp.replace(meta_path)
            except OSError as e:
                logger.warning("Failed to write reflect cache file: %s", e)

            return format_response(
                {
                    "status": "complete",
                    "total_classes": data.get("total_classes", 0),
                    "cpp_count": data.get("cpp_count", 0),
                    "blueprint_count": data.get("blueprint_count", 0),
                    "project_cpp_count": data.get("project_cpp_count", 0),
                    "engine_cpp_count": data.get("engine_cpp_count", 0),
                    "project_blueprint_count": data.get("project_blueprint_count", 0),
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
        cache_file = cache_dir / "reflect-cache.json"
        if cache_file.exists():
            try:
                cache = json.loads(cache_file.read_text())
                data = cache.get("data", {})
                age = time.time() - cache_file.stat().st_mtime
                return json.dumps(
                    {
                        "cached": True,
                        "age_seconds": int(age),
                        "class_count": data.get("total_classes", 0),
                        "cache_mode": "persistent",
                        "source": "file",
                        "stale": age > 3600,
                    },
                    indent=2,
                )
            except (json.JSONDecodeError, OSError):
                pass

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
                    "source": "meta",
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
            for file_name in ("reflect-cache.json", "meta.json"):
                file_path = cache_dir / file_name
                if file_path.exists():
                    try:
                        file_path.unlink()
                    except OSError as e:
                        logger.warning("Failed to delete cache file %s: %s", file_path, e)

        # Re-scan
        return scan_project()
