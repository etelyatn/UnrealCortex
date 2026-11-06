"""Contract tests for the agent-facing capability inventory.

`core.get_capabilities` is how a caller discovers the command surface, so two things must hold:
the names projection runs *before* the response-size guard (otherwise the whole inventory is
refused and nothing is discoverable), and an oversized full read refuses explicitly instead of
returning a partial document. A fake connection keeps these unit-level and editor-free.
"""

from __future__ import annotations

import json

import pytest

from cortex_mcp.tools.routers import make_router, strict_router_tool

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


def test_names_detail_drops_the_signature_inventory(core_cmd):
    result = json.loads(core_cmd("get_capabilities", {}))

    command = result["domains"]["domain0"]["commands"][0]
    assert command["name"] == "command_0"
    assert "params" not in command


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
