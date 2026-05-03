"""Tests for capabilities cache loading and router docstring generation."""

import json
import logging
import os
import re
from pathlib import Path

import pytest

from cortex_mcp.capabilities import (
    CORE_DOMAINS,
    _COMPOSITE_HINTS,
    build_router_docstrings,
    get_registered_domains,
    load_capabilities_cache,
    minimal_router_docstrings,
)
from cortex_mcp._fallback_generated import FALLBACK_COMMANDS as _FALLBACK_STRUCTURED


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
    assert (
        "- save_asset(items: array = optional, expected_fingerprint: object = optional, "
        "asset_path: string, force: boolean = optional, dry_run: boolean = optional)"
    ) in docstrings["core"]
    assert "query_datatable" in docstrings["data"]
    assert "table_path: string" in docstrings["data"]


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
        for domain in CORE_DOMAINS:
            assert domain in docstrings
            assert "Available commands:" in docstrings[domain]
        assert "capabilities cache" in caplog.text.lower()
    finally:
        if old_project_dir is None:
            del os.environ["CORTEX_PROJECT_DIR"]
        else:
            os.environ["CORTEX_PROJECT_DIR"] = old_project_dir


# --- get_registered_domains tests ---


def test_get_registered_domains_returns_core_when_no_cache():
    """No capabilities cache should return only core domains."""
    assert get_registered_domains(None) == CORE_DOMAINS


def test_get_registered_domains_returns_core_when_empty_domains():
    """Cache with empty domains dict should return only core domains."""
    assert get_registered_domains({"domains": {}}) == CORE_DOMAINS


def test_get_registered_domains_includes_gen_when_in_cache():
    """Cache with gen domain should include gen in registered domains."""
    capabilities = {"domains": {"gen": {"commands": []}}}
    registered = get_registered_domains(capabilities)
    assert registered == CORE_DOMAINS + ("gen",)


def test_get_registered_domains_returns_core_when_malformed():
    """Malformed cache (no domains key) should return only core domains."""
    assert get_registered_domains({"unexpected_key": True}) == CORE_DOMAINS
    assert get_registered_domains({"domains": "not_a_dict"}) == CORE_DOMAINS


# --- Minimal router docstrings tests ---


DOMAINS_WITH_COMMANDS = list(CORE_DOMAINS)


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


# --- gen-enabled registration path tests ---


def test_gen_docstrings_included_when_cache_has_gen():
    """When capabilities cache includes gen, build_router_docstrings should include gen."""
    capabilities = {"domains": {"gen": {"commands": [{"name": "start_mesh", "params": []}]}}}
    docstrings = build_router_docstrings(capabilities)
    assert "gen" in docstrings
    assert "gen_cmd" in docstrings["gen"]


def test_gen_docstrings_excluded_when_cache_missing():
    """When no capabilities cache, gen should not appear in docstrings."""
    docstrings = build_router_docstrings(None)
    assert "gen" not in docstrings


class TestFallbackDrift:
    """Detect drift between generated fallback and capabilities cache fixture.

    If these tests fail, run: cd MCP && uv run python scripts/sync_fallback.py --from-fixture
    """

    @pytest.fixture()
    def cache_domains(self) -> dict:
        data = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
        return data["domains"]

    def test_command_names_match(self, cache_domains):
        """Every command in the cache fixture must appear in the fallback, and vice versa."""
        for domain_name, domain_info in cache_domains.items():
            if domain_name not in _FALLBACK_STRUCTURED:
                continue
            cache_cmds = {cmd["name"] for cmd in domain_info.get("commands", [])}
            fallback_cmds = {cmd["name"] for cmd in _FALLBACK_STRUCTURED[domain_name]}

            missing = cache_cmds - fallback_cmds
            extra = fallback_cmds - cache_cmds
            assert not missing and not extra, (
                f"Domain '{domain_name}' command mismatch.\n"
                f"  Missing from fallback: {missing}\n"
                f"  Extra in fallback: {extra}\n"
                f"  Fix: cd MCP && uv run python scripts/sync_fallback.py --from-fixture"
            )

    def test_parameter_signatures_match(self, cache_domains):
        """Parameter names, types, and required/optional must match between cache and fallback."""
        mismatches = []
        for domain_name, domain_info in cache_domains.items():
            if domain_name not in _FALLBACK_STRUCTURED:
                continue
            fallback_by_name = {cmd["name"]: cmd for cmd in _FALLBACK_STRUCTURED[domain_name]}
            for cmd in domain_info.get("commands", []):
                cmd_name = cmd["name"]
                if cmd_name not in fallback_by_name:
                    continue  # caught by test_command_names_match
                cache_params = [
                    (p["name"], p.get("type", "any"), p.get("required", False))
                    for p in cmd.get("params", [])
                ]
                fallback_params = [
                    (p["name"], p.get("type", "any"), p.get("required", False))
                    for p in fallback_by_name[cmd_name].get("params", [])
                ]
                if cache_params != fallback_params:
                    mismatches.append(
                        f"  {domain_name}.{cmd_name}:\n"
                        f"    cache:    {cache_params}\n"
                        f"    fallback: {fallback_params}"
                    )
        assert not mismatches, (
            f"Parameter drift detected in {len(mismatches)} commands:\n"
            + "\n".join(mismatches)
            + "\n\nFix: cd MCP && uv run python scripts/sync_fallback.py --from-fixture"
        )

    def test_fallback_domains_subset_of_cache(self, cache_domains):
        """Generated fallback should not contain domains absent from cache."""
        extra = set(_FALLBACK_STRUCTURED) - set(cache_domains)
        assert not extra, (
            f"Fallback has domains not in cache: {extra}. "
            f"Regenerate from current cache."
        )


class TestCompositeHints:
    """Validate that _COMPOSITE_HINTS reference real composite tools."""

    def test_hints_reference_existing_domains(self):
        """Every domain in _COMPOSITE_HINTS must be a known domain."""
        all_domains = set(CORE_DOMAINS) | {"gen"}
        for domain in _COMPOSITE_HINTS:
            assert domain in all_domains, (
                f"_COMPOSITE_HINTS references unknown domain '{domain}'"
            )

    def test_hints_reference_compose_tools(self):
        """Hints mentioning _compose tools should reference real MCP tool names."""
        known_compose_tools = {
            "material_compose",
            "material_instance_compose",
            "blueprint_compose",
            "widget_compose",
            "level_compose",
            "gen_compose",
            "scenario_compose",
        }
        for domain, hint in _COMPOSITE_HINTS.items():
            matches = re.findall(r"(\w+_compose)", hint)
            for tool_name in matches:
                assert tool_name in known_compose_tools, (
                    f"_COMPOSITE_HINTS['{domain}'] references '{tool_name}' "
                    f"which is not a known compose tool"
                )
