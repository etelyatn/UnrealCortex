"""Tests for file-based cache loading."""

import json
import os
import pathlib
import tempfile
import time

from cortex_mcp.tcp_client import UEConnection
from cortex_mcp.cache import ResponseCache


def test_load_reflect_cache_from_file():
    """Reflect cache file should be read into in-memory ResponseCache."""
    with tempfile.TemporaryDirectory() as tmpdir:
        project_dir = pathlib.Path(tmpdir)
        cache_dir = project_dir / "Saved" / "Cortex"
        cache_dir.mkdir(parents=True)

        params = {
            "root": "AActor",
            "depth": 10,
            "max_results": 5000,
            "include_engine": False,
        }
        cache_data = {
            "timestamp": "2026-03-02T12:00:00Z",
            "params": params,
            "data": {
                "name": "AActor",
                "total_classes": 100,
                "cpp_count": 80,
                "blueprint_count": 20,
                "children": [],
            },
        }
        (cache_dir / "reflect-cache.json").write_text(json.dumps(cache_data), encoding="utf-8")

        old_project_dir = os.environ.get("CORTEX_PROJECT_DIR")
        os.environ["CORTEX_PROJECT_DIR"] = str(project_dir)
        try:
            conn = UEConnection(port=8742)
            conn.load_file_caches()
            key = ResponseCache.make_key("reflect.class_hierarchy", params)
            cached = conn._cache.get(key)
            assert cached is not None
            assert cached["data"]["total_classes"] == 100
        finally:
            if old_project_dir is None:
                del os.environ["CORTEX_PROJECT_DIR"]
            else:
                os.environ["CORTEX_PROJECT_DIR"] = old_project_dir


def test_cache_shape_validation():
    """Cache missing total_classes should be skipped."""
    with tempfile.TemporaryDirectory() as tmpdir:
        project_dir = pathlib.Path(tmpdir)
        cache_dir = project_dir / "Saved" / "Cortex"
        cache_dir.mkdir(parents=True)

        bad_cache = {"timestamp": "2026-03-02T12:00:00Z", "data": {"name": "AActor"}}
        (cache_dir / "reflect-cache.json").write_text(json.dumps(bad_cache), encoding="utf-8")

        old_project_dir = os.environ.get("CORTEX_PROJECT_DIR")
        os.environ["CORTEX_PROJECT_DIR"] = str(project_dir)
        try:
            conn = UEConnection(port=8742)
            conn.load_file_caches()
            key = ResponseCache.make_key("reflect.class_hierarchy", None)
            assert conn._cache.get(key) is None
        finally:
            if old_project_dir is None:
                del os.environ["CORTEX_PROJECT_DIR"]
            else:
                os.environ["CORTEX_PROJECT_DIR"] = old_project_dir


def test_cache_freshness_check():
    """Cache older than 1 hour should not be loaded."""
    with tempfile.TemporaryDirectory() as tmpdir:
        project_dir = pathlib.Path(tmpdir)
        cache_dir = project_dir / "Saved" / "Cortex"
        cache_dir.mkdir(parents=True)

        cache_data = {
            "timestamp": "2026-03-02T12:00:00Z",
            "params": {"root": "AActor", "depth": 10, "max_results": 5000, "include_engine": False},
            "data": {"name": "AActor", "total_classes": 100},
        }
        cache_file = cache_dir / "reflect-cache.json"
        cache_file.write_text(json.dumps(cache_data), encoding="utf-8")
        stale = time.time() - 7200
        os.utime(cache_file, (stale, stale))

        old_project_dir = os.environ.get("CORTEX_PROJECT_DIR")
        os.environ["CORTEX_PROJECT_DIR"] = str(project_dir)
        try:
            conn = UEConnection(port=8742)
            conn.load_file_caches()
            key = ResponseCache.make_key("reflect.class_hierarchy", cache_data["params"])
            assert conn._cache.get(key) is None
        finally:
            if old_project_dir is None:
                del os.environ["CORTEX_PROJECT_DIR"]
            else:
                os.environ["CORTEX_PROJECT_DIR"] = old_project_dir
