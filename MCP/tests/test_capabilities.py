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
    assert "For large raw DataTable, StringTable, or DataAsset reads" in docstrings["data"]
    assert "export_schema_json" in docstrings["data"]
    assert "export_bulk_json" in docstrings["data"]
    assert "compare_data_json" in docstrings["data"]
    assert "compact summaries" in docstrings["data"]


def test_build_router_docstring_lists_compare_data_json():
    """Data router docs should advertise compare_data_json in file-backed workflow hints."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    docstrings = build_router_docstrings(capabilities)
    assert "compare_data_json" in docstrings["data"]


def test_build_router_docstring_lists_export_schema_json():
    """Data router docs should advertise export_schema_json in file-backed workflow hints."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    docstrings = build_router_docstrings(capabilities)
    assert "export_schema_json" in docstrings["data"]


def test_editor_router_hint_warns_about_trusted_python():
    """Editor router docs should warn agents before using run_python."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))

    docstrings = build_router_docstrings(capabilities)

    assert "editor" in docstrings
    assert "high-trust escape hatch" in docstrings["editor"]
    assert "prefer structured Cortex commands" in docstrings["editor"]
    assert "mutate assets/files" in docstrings["editor"]


def test_editor_capabilities_include_python_and_cvars():
    """Editor capabilities fixture should expose trusted Python and CVar commands."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    editor_commands = {
        cmd["name"]: cmd
        for cmd in capabilities["domains"]["editor"]["commands"]
    }

    for command_name in ("run_python", "get_cvar", "set_cvar", "list_cvars"):
        assert command_name in editor_commands

    run_python_params = {param["name"]: param for param in editor_commands["run_python"]["params"]}
    assert "code" in run_python_params
    assert run_python_params["code"]["required"] is True
    assert "run_next_tick" in run_python_params
    assert "defer" not in run_python_params

    description = editor_commands["run_python"]["description"]
    assert "high-trust escape hatch" in description
    assert "prefer structured Cortex commands" in description
    assert "mutate assets/files" in description


def test_editor_docstrings_include_python_signature():
    """Editor router docs should list run_python with run_next_tick and no defer alias."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    docstrings = build_router_docstrings(capabilities)

    assert "- run_python(code: string, run_next_tick: boolean = optional)" in docstrings["editor"]
    assert "defer" not in docstrings["editor"]


def test_fallback_editor_has_python_and_cvars_without_defer():
    """Generated no-editor fallback should match the selected editor slice."""
    fallback_commands = {cmd["name"]: cmd for cmd in _FALLBACK_STRUCTURED["editor"]}

    for command_name in ("run_python", "get_cvar", "set_cvar", "list_cvars"):
        assert command_name in fallback_commands

    run_python_params = {param["name"] for param in fallback_commands["run_python"].get("params", [])}
    assert "code" in run_python_params
    assert "run_next_tick" in run_python_params
    assert "defer" not in run_python_params


def test_anim_absent_from_fallback_fixture_until_promoted():
    """Phase A anim is live-capabilities only; fallback promotion needs an explicit fixture update."""
    fixture = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))

    assert "anim" not in fixture.get("domains", {})
    assert "animation" not in fixture.get("domains", {})
    assert "anim" not in _FALLBACK_STRUCTURED
    assert "animation" not in _FALLBACK_STRUCTURED


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


def test_anim_is_optional_domain_not_core():
    """Animation is exposed only when live capabilities prove the editor registered anim."""
    assert "anim" not in CORE_DOMAINS
    assert get_registered_domains(None) == CORE_DOMAINS


def test_get_registered_domains_includes_anim_when_in_cache():
    """Cache with anim domain should include anim in registered domains."""
    capabilities = {"domains": {"anim": {"commands": []}}}
    registered = get_registered_domains(capabilities)
    assert registered == CORE_DOMAINS + ("anim",)


def test_get_registered_domains_returns_core_when_malformed():
    """Malformed cache (no domains key) should return only core domains."""
    assert get_registered_domains({"unexpected_key": True}) == CORE_DOMAINS
    assert get_registered_domains({"domains": "not_a_dict"}) == CORE_DOMAINS


def test_statetree_is_core_domain():
    """StateTree should be exposed as a core router domain with a compose hint."""
    assert "statetree" in CORE_DOMAINS
    assert "statetree" in _COMPOSITE_HINTS
    assert "statetree_compose" in _COMPOSITE_HINTS["statetree"]


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


def test_anim_docstrings_excluded_when_cache_missing():
    """No capabilities cache should not expose anim_cmd."""
    docstrings = build_router_docstrings(None)
    assert "anim" not in docstrings


def test_anim_docstrings_included_when_cache_has_anim():
    """A live capabilities cache with anim should expose anim_cmd and exact live command signatures."""
    capabilities = {
        "domains": {
            "anim": {
                "commands": [
                    {
                        "name": "get_sequence_info",
                        "params": [
                            {"name": "asset_path", "type": "string", "required": True},
                            {"name": "notify_limit", "type": "number", "required": False},
                        ],
                    },
                    {
                        "name": "add_named_notify",
                        "params": [
                            {"name": "asset_path", "type": "string", "required": True},
                            {"name": "notify_name", "type": "string", "required": True},
                            {"name": "time", "type": "number", "required": True},
                            {"name": "expected_fingerprint", "type": "object", "required": True},
                            {"name": "dry_run", "type": "boolean", "required": False},
                            {"name": "save", "type": "boolean", "required": False},
                        ],
                    },
                    {
                        "name": "update_named_notify",
                        "params": [
                            {"name": "asset_path", "type": "string", "required": True},
                            {"name": "selector", "type": "object", "required": True},
                            {"name": "expected_fingerprint", "type": "object", "required": True},
                        ],
                    },
                    {
                        "name": "remove_named_notify",
                        "params": [
                            {"name": "asset_path", "type": "string", "required": True},
                            {"name": "selector", "type": "object", "required": True},
                            {"name": "expected_fingerprint", "type": "object", "required": True},
                        ],
                    },
                ]
            }
        }
    }

    docstrings = build_router_docstrings(capabilities)

    assert "anim" in get_registered_domains(capabilities)
    assert "anim" in docstrings
    assert "anim_cmd(command, params)" in docstrings["anim"]
    assert "- get_sequence_info(asset_path: string, notify_limit: number = optional)" in docstrings["anim"]
    assert (
        "- add_named_notify(asset_path: string, notify_name: string, time: number, "
        "expected_fingerprint: object, dry_run: boolean = optional, save: boolean = optional)"
    ) in docstrings["anim"]
    assert "sequence named-notify authoring" in docstrings["anim"]
    assert "Only call animation authoring commands that appear in live capabilities" in docstrings["anim"]
    for authoring_name in ("add_notify", "update_notify", "remove_notify", "set_curve_keys"):
        assert authoring_name not in docstrings["anim"]
    assert "- save_asset(" not in docstrings["anim"]


def _fake_anim_capabilities(command_names: list[str]) -> dict:
    common_asset = [{"name": "asset_path", "type": "string", "required": True}]
    expected = {"name": "expected_fingerprint", "type": "object", "required": True}
    optional_mutation = [
        {"name": "dry_run", "type": "boolean", "required": False},
        {"name": "save", "type": "boolean", "required": False},
    ]
    definitions = {
        "add_named_notify": common_asset + [
            {"name": "notify_name", "type": "string", "required": True},
            {"name": "time", "type": "number", "required": True},
            expected,
            *optional_mutation,
        ],
        "update_named_notify": common_asset + [
            {"name": "selector", "type": "object", "required": True},
            expected,
            {"name": "new_name", "type": "string", "required": False},
            {"name": "new_time", "type": "number", "required": False},
            *optional_mutation,
        ],
        "remove_named_notify": common_asset + [
            {"name": "selector", "type": "object", "required": True}, expected, *optional_mutation
        ],
        "add_curve": common_asset + [
            {"name": "curve_name", "type": "string", "required": True}, expected, *optional_mutation
        ],
        "set_curve_keys": common_asset + [
            {"name": "curve_name", "type": "string", "required": True},
            {"name": "keys", "type": "array", "required": True},
            expected,
            *optional_mutation,
        ],
        "remove_curve": common_asset + [
            {"name": "curve_name", "type": "string", "required": True}, expected, *optional_mutation
        ],
        "add_montage_section": common_asset + [
            {"name": "name", "type": "string", "required": True},
            {"name": "start_time", "type": "number", "required": True},
            expected,
            {"name": "next_section", "type": "string", "required": False},
            *optional_mutation,
        ],
        "update_montage_section": common_asset + [
            {"name": "selector", "type": "object", "required": True},
            expected,
            {"name": "new_name", "type": "string", "required": False},
            {"name": "new_start_time", "type": "number", "required": False},
            {"name": "new_next_section", "type": "string", "required": False},
            *optional_mutation,
        ],
        "remove_montage_section": common_asset + [
            {"name": "selector", "type": "object", "required": True}, expected, *optional_mutation
        ],
        "add_socket": common_asset + [
            {"name": "socket_name", "type": "string", "required": True},
            {"name": "bone_name", "type": "string", "required": True},
            expected,
            {"name": "location", "type": "object", "required": False},
            {"name": "rotation", "type": "object", "required": False},
            {"name": "scale", "type": "object", "required": False},
            *optional_mutation,
        ],
        "set_socket_transform": common_asset + [
            {"name": "selector", "type": "object", "required": True},
            expected,
            {"name": "location", "type": "object", "required": False},
            {"name": "rotation", "type": "object", "required": False},
            {"name": "scale", "type": "object", "required": False},
            *optional_mutation,
        ],
        "remove_socket": common_asset + [
            {"name": "selector", "type": "object", "required": True}, expected, *optional_mutation
        ],
        "add_notify": common_asset + [
            {"name": "notify_class_path", "type": "string", "required": True},
            {"name": "time", "type": "number", "required": True}, expected, *optional_mutation,
        ],
        "update_notify": common_asset + [
            {"name": "selector", "type": "object", "required": True}, expected,
            {"name": "new_time", "type": "number", "required": True}, *optional_mutation,
        ],
        "remove_notify": common_asset + [
            {"name": "selector", "type": "object", "required": True}, expected, *optional_mutation,
        ],
        "add_notify_state": common_asset + [
            {"name": "notify_state_class_path", "type": "string", "required": True},
            {"name": "start_time", "type": "number", "required": True},
            {"name": "duration", "type": "number", "required": True}, expected, *optional_mutation,
        ],
        "update_notify_state": common_asset + [
            {"name": "selector", "type": "object", "required": True}, expected,
            {"name": "new_start_time", "type": "number", "required": False},
            {"name": "new_duration", "type": "number", "required": False}, *optional_mutation,
        ],
        "remove_notify_state": common_asset + [
            {"name": "selector", "type": "object", "required": True}, expected, *optional_mutation,
        ],
    }
    return {
        "domains": {
            "anim": {
                "commands": [{"name": name, "params": definitions[name]} for name in command_names]
            }
        }
    }


def test_anim_docstrings_advertise_only_complete_phase_b2_families():
    """Live anim hints advertise all twelve authoring commands only as complete families."""
    command_names = [
        "add_named_notify", "update_named_notify", "remove_named_notify",
        "add_curve", "set_curve_keys", "remove_curve",
        "add_montage_section", "update_montage_section", "remove_montage_section",
        "add_socket", "set_socket_transform", "remove_socket",
    ]
    docstring = build_router_docstrings(_fake_anim_capabilities(command_names))["anim"]

    for signature in (
        "- add_curve(asset_path: string, curve_name: string, expected_fingerprint: object, dry_run: boolean = optional, save: boolean = optional)",
        "- set_curve_keys(asset_path: string, curve_name: string, keys: array, expected_fingerprint: object, dry_run: boolean = optional, save: boolean = optional)",
        "- add_montage_section(asset_path: string, name: string, start_time: number, expected_fingerprint: object, next_section: string = optional, dry_run: boolean = optional, save: boolean = optional)",
        "- update_montage_section(asset_path: string, selector: object, expected_fingerprint: object, new_name: string = optional, new_start_time: number = optional, new_next_section: string = optional, dry_run: boolean = optional, save: boolean = optional)",
        "- add_socket(asset_path: string, socket_name: string, bone_name: string, expected_fingerprint: object, location: object = optional, rotation: object = optional, scale: object = optional, dry_run: boolean = optional, save: boolean = optional)",
        "- set_socket_transform(asset_path: string, selector: object, expected_fingerprint: object, location: object = optional, rotation: object = optional, scale: object = optional, dry_run: boolean = optional, save: boolean = optional)",
    ):
        assert signature in docstring
    assert "save defaults to false" in docstring
    assert "selector { index, socket_name, bone_name }" in docstring
    assert "Do not invent later-stage AnimBP authoring" in docstring
    assert "add_notify" not in docstring


def test_anim_docstrings_advertise_only_complete_phase_c_families():
    commands = [
        "add_notify", "update_notify", "remove_notify",
        "add_notify_state", "update_notify_state", "remove_notify_state",
    ]
    docstring = build_router_docstrings(_fake_anim_capabilities(commands))["anim"]
    assert "- add_notify(asset_path: string, notify_class_path: string, time: number, expected_fingerprint: object, dry_run: boolean = optional, save: boolean = optional)" in docstring
    assert "- add_notify_state(asset_path: string, notify_state_class_path: string, start_time: number, duration: number, expected_fingerprint: object, dry_run: boolean = optional, save: boolean = optional)" in docstring
    assert "set_curve_keys" not in docstring
    assert "save_asset" not in docstring


@pytest.mark.parametrize(
    "partial_family",
    [
        ["add_notify", "update_notify"],
        ["add_notify_state", "remove_notify_state"],
    ],
)
def test_anim_docstrings_do_not_promote_partial_phase_c_family(partial_family):
    """Object-notify and notify-state families are independently capability-gated."""
    docstring = build_router_docstrings(_fake_anim_capabilities(partial_family))["anim"]
    for command_name in (
        "add_notify", "update_notify", "remove_notify",
        "add_notify_state", "update_notify_state", "remove_notify_state",
    ):
        assert command_name not in docstring


def test_anim_docstrings_do_not_promote_incomplete_family():
    """A partial live cache must not advertise any incomplete B2 family."""
    docstring = build_router_docstrings(_fake_anim_capabilities(["add_curve"]))["anim"]
    for command_name in ("add_curve", "set_curve_keys", "remove_curve", "add_montage_section", "add_socket"):
        assert command_name not in docstring


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

        expected_data_exports = {
            "export_datatable_json",
            "export_string_table_json",
            "export_data_assets_json",
            "export_schema_json",
            "export_bulk_json",
            "compare_data_json",
        }
        cache_data_cmds = {cmd["name"] for cmd in cache_domains["data"].get("commands", [])}
        fallback_data_cmds = {cmd["name"] for cmd in _FALLBACK_STRUCTURED["data"]}

        assert expected_data_exports <= cache_data_cmds
        assert expected_data_exports <= fallback_data_cmds

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

def test_blueprint_set_class_defaults_exposes_batch_items():
    """The AI-facing fallback must advertise set_class_defaults batch mode."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    blueprint_commands = capabilities["domains"]["blueprint"]["commands"]
    cache_command = next(cmd for cmd in blueprint_commands if cmd["name"] == "set_class_defaults")
    fallback_command = next(cmd for cmd in _FALLBACK_STRUCTURED["blueprint"] if cmd["name"] == "set_class_defaults")

    cache_params = {param["name"] for param in cache_command["params"]}
    fallback_params = {param["name"] for param in fallback_command["params"]}

    assert "items" in cache_params
    assert "items" in fallback_params


def test_graph_set_pin_value_capability_includes_typed_text():
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    graph_commands = capabilities["domains"]["graph"]["commands"]
    set_pin = next(command for command in graph_commands if command["name"] == "set_pin_value")
    params = {param["name"]: param for param in set_pin["params"]}

    assert params["value"]["required"] is False
    assert params["text"]["type"] == "object"
    assert params["text"]["required"] is False
    assert params["expected_fingerprint"]["type"] == "object"
    assert params["graph_kind"]["required"] is False
    assert params["owning_interface"]["required"] is False


class TestCompositeHints:
    """Validate that _COMPOSITE_HINTS reference real composite tools."""

    def test_hints_reference_existing_domains(self):
        """Every domain in _COMPOSITE_HINTS must be a known domain."""
        all_domains = set(CORE_DOMAINS) | {"gen", "anim"}
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
            "statetree_compose",
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
