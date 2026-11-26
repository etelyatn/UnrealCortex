"""Contract tests for the agent-facing capability inventory.

`core.get_capabilities` is how a caller discovers the command surface, so two things must hold:
the names projection runs *before* the response-size guard (otherwise the whole inventory is
refused and nothing is discoverable), and an oversized full read refuses explicitly instead of
returning a partial document. A fake connection keeps these unit-level and editor-free.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from cortex_mcp.tools.routers import make_router, strict_router_tool

FIXTURES_DIR = Path(__file__).parent / "fixtures"

# Sized so the raw document exceeds the 40 KB response budget while the names projection of it
# stays comfortably inside it - that gap is exactly what the ordering defect erased.
_DOMAINS = 4
_COMMANDS_PER_DOMAIN = 15
_PARAMS_PER_COMMAND = 10


def _capability_document() -> dict:
    return {
        "plugin_version": "0.0.0-test",
        "domains": {
            f"domain{i}": {
                "name": f"Domain {i}",
                "version": "1.0.0",
                "commands": [
                    {
                        "name": f"command_{j}",
                        "description": "d" * 100,
                        "params": [
                            {
                                "name": f"param_{k}",
                                "type": "string",
                                "required": False,
                                "description": "p" * 150,
                            }
                            for k in range(_PARAMS_PER_COMMAND)
                        ],
                    }
                    for j in range(_COMMANDS_PER_DOMAIN)
                ],
            }
            for i in range(_DOMAINS)
        },
    }


class _FakeConnection:
    """Returns the capability payload for any read; the router path is what is under test."""

    def __init__(self, payload: dict):
        self.payload = payload

    def send_command(self, command, params=None, timeout=None):
        return self.payload

    def send_command_cached(self, command, params=None, ttl=300.0, timeout=None):
        return self.payload

    def record_tool_invocation(self, *args, **kwargs):
        pass


@pytest.fixture
def core_cmd():
    payload = {"success": True, "data": _capability_document()}
    return strict_router_tool(make_router("core", _FakeConnection(payload), ""), "core")


def test_fixture_exceeds_the_response_budget_before_projection():
    raw = json.dumps(_capability_document(), separators=(",", ":"))
    assert len(raw) > 40000, "the fixture must be over budget for these tests to mean anything"


@pytest.mark.parametrize("params", [{}, {"detail": "names"}])
def test_names_detail_returns_the_projection_instead_of_refusing(core_cmd, params):
    result = json.loads(core_cmd("get_capabilities", params))

    assert "_error" not in result, result
    assert set(result["domains"]) == {f"domain{i}" for i in range(_DOMAINS)}


def test_names_detail_lists_only_command_names(core_cmd):
    result = json.loads(core_cmd("get_capabilities", {}))

    body = result["domains"]["domain0"]
    assert body["name"] == "Domain 0"
    assert body["commands"] == [f"command_{j}" for j in range(_COMMANDS_PER_DOMAIN)]


def test_names_detail_of_the_recorded_inventory_fits_the_budget():
    """Issue 262: the recorded editor inventory's names view must not refuse as too large."""
    document = json.loads((FIXTURES_DIR / "capabilities_cache_full.json").read_text(encoding="utf-8"))
    core = strict_router_tool(
        make_router("core", _FakeConnection({"success": True, "data": document}), ""), "core"
    )

    result = json.loads(core("get_capabilities", {"detail": "names"}))

    assert "_error" not in result, result.get("_error")
    assert result["domains"]["material"]["commands"] == [
        command["name"] for command in document["domains"]["material"]["commands"]
    ]


@pytest.mark.parametrize("detail", ["names", "full"])
def test_domain_selects_one_domain(core_cmd, detail):
    result = json.loads(core_cmd("get_capabilities", {"domain": "domain2", "detail": detail}))

    assert "_error" not in result, result
    assert list(result["domains"]) == ["domain2"]
    assert result["plugin_version"] == "0.0.0-test"


def test_domain_full_keeps_signatures(core_cmd):
    result = json.loads(core_cmd("get_capabilities", {"domain": "domain1", "detail": "full"}))

    command = result["domains"]["domain1"]["commands"][0]
    assert command["name"] == "command_0"
    assert len(command["params"]) == _PARAMS_PER_COMMAND


@pytest.mark.parametrize("domain", ["missing", "", 7])
def test_unknown_domain_is_refused_with_the_available_domains(core_cmd, domain):
    result = json.loads(core_cmd("get_capabilities", {"domain": domain}))

    assert result["_error"] == "INVALID_FIELD"
    assert "domain0" in result["_message"] and "domain3" in result["_message"]


def test_domain_selector_is_not_forwarded_to_the_editor():
    seen: list = []

    class _Recording(_FakeConnection):
        def send_command_cached(self, command, params=None, ttl=300.0, timeout=None):
            seen.append((command, params))
            return self.payload

    core = strict_router_tool(
        make_router("core", _Recording({"success": True, "data": _capability_document()}), ""),
        "core",
    )
    json.loads(core("get_capabilities", {"domain": "domain0"}))

    assert seen == [("get_capabilities", {})]


def test_invalid_detail_is_refused(core_cmd):
    result = json.loads(core_cmd("get_capabilities", {"detail": "bogus"}))

    assert result["_error"] == "INVALID_FIELD"
    assert "detail" in result["_message"]


def test_oversized_full_read_refuses_without_inventing_a_selector(core_cmd):
    result = json.loads(core_cmd("get_capabilities", {"detail": "full"}))

    assert result["_error"] == "RESPONSE_TOO_LARGE"
    assert result.get("reader_complete") is False
    # This command declares no limit/cursor, so a refusal must not advise one.
    assert "limit" not in json.dumps(result).lower()
    # The refusal names the selectors that do narrow it.
    assert "domain" in json.dumps(result)
