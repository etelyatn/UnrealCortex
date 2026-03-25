"""Tests for capabilities cache loading and router docstring generation."""

import json
import logging
import os
from pathlib import Path

import pytest

from cortex_mcp.capabilities import (
    _DOMAINS,
    build_router_docstrings,
    load_capabilities_cache,
    minimal_router_docstrings,
)


FIXTURES_DIR = Path(__file__).parent / "fixtures"


def test_load_capabilities_cache_reads_saved_cortex_file(tmp_path):
    """Capabilities cache should be read from Saved/Cortex/capabilities-cache.json."""
    project_dir = tmp_path
    cache_dir = project_dir / "Saved" / "Cortex"
    cache_dir.mkdir(parents=True)

    expected = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    (cache_dir / "capabilities-cache.json").write_text(json.dumps(expected), encoding="utf-8")

    old_project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    os.environ["CORTEX_PROJECT_DIR"] = str(project_dir)
    try:
        loaded = load_capabilities_cache()
        assert loaded == expected
    finally:
        if old_project_dir is None:
            del os.environ["CORTEX_PROJECT_DIR"]
        else:
            os.environ["CORTEX_PROJECT_DIR"] = old_project_dir


def test_build_router_docstring_lists_all_commands():
    """Generated router docstrings should list every command with signature-style params."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))

    docstrings = build_router_docstrings(capabilities)

    assert "core" in docstrings
    assert "core_cmd(command, params)" in docstrings["core"]
    assert "- get_status()" in docstrings["core"]
    assert "- save_asset(asset_path: string, only_if_is_dirty: boolean = optional)" in docstrings["core"]
    assert "- query_datatable(table_path: string, row_filter: string = optional)" in docstrings["data"]


def test_missing_cache_uses_minimal_fallback_and_logs_warning(caplog, tmp_path):
    """Missing cache should fall back to minimal router docs and log a warning."""
    old_project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    os.environ["CORTEX_PROJECT_DIR"] = str(tmp_path)
    try:
        with caplog.at_level(logging.WARNING):
            loaded = load_capabilities_cache()
            docstrings = build_router_docstrings(loaded)

        assert loaded is None
        minimal = minimal_router_docstrings()
        assert docstrings == minimal
        for domain in _DOMAINS:
            assert domain in docstrings
            assert "Available commands:" in docstrings[domain]
        assert "capabilities cache" in caplog.text.lower()
    finally:
        if old_project_dir is None:
            del os.environ["CORTEX_PROJECT_DIR"]
        else:
            os.environ["CORTEX_PROJECT_DIR"] = old_project_dir


DOMAINS_WITH_COMMANDS = list(_DOMAINS)


class TestMinimalRouterDocstrings:
    """All domains must have command lists in fallback docstrings."""

    @pytest.mark.parametrize("domain", DOMAINS_WITH_COMMANDS)
    def test_domain_has_command_list(self, domain: str):
        docstrings = minimal_router_docstrings()
        assert domain in docstrings, f"Missing docstring for domain '{domain}'"
        assert "Available commands:" in docstrings[domain], (
            f"Domain '{domain}' fallback docstring missing 'Available commands:' section"
        )

    @pytest.mark.parametrize("domain", DOMAINS_WITH_COMMANDS)
    def test_domain_has_at_least_one_command(self, domain: str):
        docstrings = minimal_router_docstrings()
        text = docstrings[domain]
        idx = text.index("Available commands:")
        commands_section = text[idx:]
        command_lines = [l for l in commands_section.splitlines() if l.strip().startswith("- ")]
        assert len(command_lines) >= 1, (
            f"Domain '{domain}' fallback has no command entries"
        )


class TestBuildRouterDocstringsNoneCache:
    """build_router_docstrings with None cache should use minimal fallback."""

    def test_none_cache_returns_minimal_with_commands(self):
        docstrings = build_router_docstrings(None)
        for domain in DOMAINS_WITH_COMMANDS:
            assert "Available commands:" in docstrings[domain]

    def test_empty_cache_returns_minimal_with_commands(self):
        docstrings = build_router_docstrings({})
        for domain in DOMAINS_WITH_COMMANDS:
            assert "Available commands:" in docstrings[domain]
