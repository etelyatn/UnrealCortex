"""Tests for capabilities cache loading and router docstring generation."""

import json
import logging
import os
import re
from pathlib import Path
from unittest.mock import MagicMock

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
from cortex_mcp.tools.routers import make_router


FIXTURES_DIR = Path(__file__).parent / "fixtures"

# Public descriptions publish the routing envelope plus complete command *names*. Parameter
# signatures (``dry_run: boolean = optional``) are not part of that contract any more, so this
# pattern must never appear in a cache or fallback description.
_PARAMETER_SIGNATURE = re.compile(
    r"\b[a-z_][a-z0-9_]*: (?:string|boolean|number|integer|array|object|any)\b"
)


def _command(domain: str, name: str, source):
    return next(cmd for cmd in source[domain] if cmd["name"] == name)


def test_remove_graph_fallback_contract_is_explicit():
    cmd = _command("blueprint", "remove_graph", _FALLBACK_STRUCTURED)
    params = {p["name"]: p for p in cmd["params"]}

    for name in ("asset_path", "name", "dry_run", "compile", "save"):
        assert params[name]["required"] is True

    assert params["dry_run"]["type"] == "boolean"
    assert params["compile"]["type"] == "boolean"
    assert params["save"]["type"] == "boolean"
    assert params["cascade_exec_chain"] == {
        "name": "cascade_exec_chain", "type": "boolean", "required": False
    }
    assert params["expected_fingerprint"]["type"] == "object"
    assert params["expected_validation_hash"]["type"] == "string"


def test_capabilities_fixture_reports_current_plugin_version():
    fixture = json.loads(
        (FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8")
    )
    assert fixture["plugin_version"] == "0.3.0"


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
    """Generated router docstrings should list every command name for the domain."""
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))

    docstrings = build_router_docstrings(capabilities)

    assert "core" in docstrings
    assert "core_cmd(command, params)" in docstrings["core"]
    assert "save_asset" in docstrings["core"]
    assert "query_datatable" in docstrings["data"]
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


def test_fallback_editor_has_python_and_cvars_without_defer():
    """Generated no-editor fallback should match the selected editor slice."""
    fallback_commands = {cmd["name"]: cmd for cmd in _FALLBACK_STRUCTURED["editor"]}

    for command_name in ("run_python", "get_cvar", "set_cvar", "list_cvars"):
        assert command_name in fallback_commands

    run_python_params = {param["name"] for param in fallback_commands["run_python"].get("params", [])}
    assert "code" in run_python_params
    assert "run_next_tick" in run_python_params
    assert "defer" not in run_python_params


def test_fallback_batch_query_exposes_rollback_controls():
    """Fallback core batch_query metadata must advertise the failure-atomic controls so a
    no-cache session can still invoke rollback-enabled batches."""
    fallback_commands = {cmd["name"]: cmd for cmd in _FALLBACK_STRUCTURED["core"]}
    batch_params = {param["name"] for param in fallback_commands["batch_query"].get("params", [])}
    assert "commands" in batch_params
    assert "steps" in batch_params
    assert "stop_on_error" in batch_params
    assert "rollback_on_error" in batch_params
    assert "verify_rollback" in batch_params


def test_fallback_graph_includes_describe_node():
    """Fallback graph metadata must expose describe_node alongside the authoring commands."""
    command_names = {cmd["name"] for cmd in _FALLBACK_STRUCTURED["graph"]}
    assert "describe_node" in command_names


def test_fallback_umg_includes_set_widget_variable():
    """Fallback umg metadata must expose the is_variable repair command."""
    command_names = {cmd["name"] for cmd in _FALLBACK_STRUCTURED["umg"]}
    assert "set_widget_variable" in command_names


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
            for command in _FALLBACK_STRUCTURED[domain]:
                assert command["name"] in docstrings[domain]
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
    """Fallback descriptions publish the router envelope plus every valid command name."""

    @pytest.mark.parametrize("domain", DOMAINS_WITH_COMMANDS)
    def test_domain_has_command_list(self, domain: str):
        docstrings = minimal_router_docstrings()
        assert domain in docstrings, f"Missing docstring for domain '{domain}'"
        assert f"{domain}_cmd(command, params)" in docstrings[domain], (
            f"Domain '{domain}' fallback docstring must publish the canonical router envelope"
        )

    @pytest.mark.parametrize("domain", DOMAINS_WITH_COMMANDS)
    def test_domain_lists_every_fallback_command_name(self, domain: str):
        text = minimal_router_docstrings()[domain]
        names = {command["name"] for command in _FALLBACK_STRUCTURED[domain]}
        assert names, f"Domain '{domain}' has no fallback command metadata"
        for name in names:
            assert name in text, f"Domain '{domain}' fallback omits command '{name}'"

    @pytest.mark.parametrize("domain", DOMAINS_WITH_COMMANDS)
    def test_domain_drops_parameter_signatures(self, domain: str):
        text = minimal_router_docstrings()[domain]
        assert " = optional" not in text
        assert _PARAMETER_SIGNATURE.search(text) is None


class TestBuildRouterDocstringsNoneCache:
    """A missing/empty capabilities cache uses the minimal fallback descriptions verbatim."""

    def test_none_cache_returns_minimal_with_commands(self):
        assert build_router_docstrings(None) == minimal_router_docstrings()

    def test_empty_cache_returns_minimal_with_commands(self):
        assert build_router_docstrings({}) == minimal_router_docstrings()


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
    """A live capabilities cache with anim should expose anim_cmd and its live command names."""
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
    assert "get_sequence_info" in docstrings["anim"]
    assert "add_named_notify" in docstrings["anim"]
    assert "sequence named-notify authoring" in docstrings["anim"]
    assert "Only call animation authoring commands that appear in live capabilities" in docstrings["anim"]
    for authoring_name in ("add_notify", "update_notify", "remove_notify", "set_curve_keys"):
        assert authoring_name not in docstrings["anim"]
    assert "save_asset" not in docstrings["anim"]


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

    for command_name in command_names:
        assert command_name in docstring
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
    assert "add_notify" in docstring
    assert "add_notify_state" in docstring
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


def test_live_operation_schema_commands_advertised():
    fixture = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    core_cmds = {cmd["name"] for cmd in fixture["domains"]["core"]["commands"]}
    graph_cmds = {cmd["name"] for cmd in fixture["domains"]["graph"]["commands"]}
    umg_cmds = {cmd["name"] for cmd in fixture["domains"]["umg"]["commands"]}
    assert "get_operation_schema" in core_cmds
    assert "describe_node" in graph_cmds
    assert "set_widget_variable" in umg_cmds
    fallback_core = {cmd["name"] for cmd in _FALLBACK_STRUCTURED["core"]}
    assert "get_operation_schema" in fallback_core


def test_batch_query_aliases_are_not_independently_required():
    fixture = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    fixture_batch = next(
        command for command in fixture["domains"]["core"]["commands"]
        if command["name"] == "batch_query"
    )
    fallback_batch = next(
        command for command in _FALLBACK_STRUCTURED["core"]
        if command["name"] == "batch_query"
    )

    for batch_contract in (fixture_batch, fallback_batch):
        params = {param["name"]: param for param in batch_contract["params"]}
        assert params["commands"]["required"] is False
        assert params["steps"]["required"] is False


def test_core_router_hint_requires_live_editor_proof():
    capabilities = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    docstrings = build_router_docstrings(capabilities)
    assert "never proves a command is executable" in docstrings["core"]
    assert "graph.describe_node before graph.add_node" in docstrings["core"]


# --- cached description rendering (names-only) ---


def _full_cache() -> dict:
    return json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))


def test_cached_router_descriptions_list_every_command_name():
    """Cache-backed descriptions must still advertise every registered command name."""
    capabilities = _full_cache()
    docstrings = build_router_docstrings(capabilities)
    for domain, domain_info in capabilities["domains"].items():
        for command in domain_info.get("commands", []):
            assert command["name"] in docstrings[domain], (domain, command["name"])


def test_cached_router_descriptions_drop_parameter_signatures():
    """Cache-backed descriptions carry names only; the signature metadata stays in the cache."""
    capabilities = _full_cache()
    docstrings = build_router_docstrings(capabilities)
    for domain, text in docstrings.items():
        assert " = optional" not in text, domain
        assert _PARAMETER_SIGNATURE.search(text) is None, domain
    # The cache itself keeps the parameter metadata the short description omits.
    core_save_asset = next(
        command
        for command in capabilities["domains"]["core"]["commands"]
        if command["name"] == "save_asset"
    )
    assert core_save_asset["params"], "signature metadata must not be stripped from the cache"


# --- agent-facing capability inventory (names-only projection) ---


def _capability_document() -> dict:
    """Fixture-derived capability document that fits the MCP response budget.

    The complete live document (~118k compact characters) exceeds MAX_RESPONSE_CHARS, so the
    facade regressions use the fixture's real records for two domains: that exercises the names
    projection and the full inventory instead of the size refusal.
    """
    full = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    return {
        "plugin_version": full["plugin_version"],
        "domains": {name: full["domains"][name] for name in ("core", "data")},
    }


def test_core_names_view_never_truncates_the_saved_capabilities_cache(tmp_path, monkeypatch):
    """core_cmd(get_capabilities) detail=names is a response projection only.

    UEConnection's handshake and the Saved cache keep the complete signature inventory, so the
    projected read cannot leave the process without the full capability data.
    """
    capabilities = _capability_document()
    cache_dir = tmp_path / "Saved" / "Cortex"
    cache_dir.mkdir(parents=True)
    (cache_dir / "capabilities-cache.json").write_text(json.dumps(capabilities), encoding="utf-8")

    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": capabilities}
    connection.send_command_cached.return_value = {"success": True, "data": capabilities}
    router = make_router("core", connection, "core docs")

    monkeypatch.setenv("CORTEX_PROJECT_DIR", str(tmp_path))

    names_view = json.loads(router("get_capabilities", {"detail": "names"}))
    assert names_view.get("_error") is None, names_view
    assert sorted(names_view["domains"]) == sorted(capabilities["domains"])
    assert all(
        "params" not in command
        for domain in names_view["domains"].values()
        for command in domain["commands"]
    )

    # The projected read is never written back: the Saved cache stays complete.
    cached = load_capabilities_cache()
    assert cached == capabilities
    cached_save_asset = next(
        command
        for command in cached["domains"]["core"]["commands"]
        if command["name"] == "save_asset"
    )
    assert cached_save_asset["params"], "the Saved cache keeps the full signature inventory"

    full_view = json.loads(router("get_capabilities", {"detail": "full"}))
    assert full_view.get("_error") is None, full_view
    core_commands = {
        command["name"]: command for command in full_view["domains"]["core"]["commands"]
    }
    assert core_commands["save_asset"]["params"], "detail=full must keep the signature inventory"


# --- startup toolset presets (design §2.2) ---

REFLECT_STANDALONE_TOOL_NAMES = frozenset({
    "scan_project",
    "reflect_cache_status",
    "rebuild_graph_cache",
    "query_class_context",
    "get_dependencies",
    "query_class_detail",
    "query_class_hierarchy",
    "impact_analysis",
    "query_overrides",
    "get_referencers",
    "query_usages",
})

# ui: core/data/blueprint/graph/umg/editor routers; blueprint_compose, widget_compose;
# profile_operation_schema, schema_generate, editor_restart.
UI_TOOLSET = frozenset({
    "core_cmd", "data_cmd", "blueprint_cmd", "graph_cmd", "umg_cmd", "editor_cmd",
    "blueprint_compose", "widget_compose",
    "profile_operation_schema", "schema_generate", "editor_restart",
})

# blueprint: core/data/blueprint/graph/editor/reflect routers; blueprint_compose; all 11
# Reflect standalone tools; profile_operation_schema, schema_generate, editor_restart.
BLUEPRINT_TOOLSET = frozenset({
    "core_cmd", "data_cmd", "blueprint_cmd", "graph_cmd", "editor_cmd", "reflect_cmd",
    "blueprint_compose",
    "profile_operation_schema", "schema_generate", "editor_restart",
}) | REFLECT_STANDALONE_TOOL_NAMES

# data: core/data/editor routers; apply_import_ops_json, schema_generate, editor_restart.
DATA_TOOLSET = frozenset({
    "core_cmd", "data_cmd", "editor_cmd",
    "apply_import_ops_json", "schema_generate", "editor_restart",
})

# ``all`` (the default) is the full registered surface for a project without optional domains.
DEFAULT_TOOLSET_SURFACE = frozenset(
    {f"{domain}_cmd" for domain in CORE_DOMAINS}
    | {
        "blueprint_compose", "material_compose", "material_instance_compose",
        "widget_compose", "level_compose", "scenario_compose", "statetree_compose",
        "editor_restart", "profile_operation_schema", "qa_test_step", "schema_generate",
        "apply_import_ops_json",
    }
    | REFLECT_STANDALONE_TOOL_NAMES
)


def _resolve_toolset(name, registered_domains):
    from cortex_mcp.capabilities import resolve_toolset

    return resolve_toolset(name, registered_domains)


def test_resolve_toolset_all_is_no_filter():
    assert _resolve_toolset("all", CORE_DOMAINS) is None


def test_resolve_toolset_returns_the_exact_design_presets():
    assert _resolve_toolset("ui", CORE_DOMAINS) == UI_TOOLSET
    assert _resolve_toolset("blueprint", CORE_DOMAINS) == BLUEPRINT_TOOLSET
    assert _resolve_toolset("data", CORE_DOMAINS) == DATA_TOOLSET


def test_resolve_toolset_refuses_an_unknown_name():
    with pytest.raises(ValueError):
        _resolve_toolset("bogus", CORE_DOMAINS)


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
