"""Tests for call-count reduction telemetry and agent prompt discipline."""

from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace

from cortex_mcp import server
from cortex_mcp.tcp_client import UEConnection


_WORKSPACE_ROOT = Path(__file__).resolve().parents[4]


def run_canonical_ripper_scenario() -> SimpleNamespace:
    """Simulate the intended post-migration call budget with connection telemetry."""
    conn = UEConnection(port=99999)
    conn.connect = lambda: None
    conn._send_and_receive = lambda command, params, timeout=None: {
        "success": True,
        "data": {"command": command, "params": params or {}},
    }

    steps = [
        ("blueprint_cmd", "graph.trace_exec", {"asset_path": "/Game/Ripper/BP_Lift"}, True, True),
        ("blueprint_cmd", "blueprint.list_scs_components", {"asset_path": "/Game/Ripper/BP_Lift"}, True, True),
        ("blueprint_cmd", "blueprint.list_settable_defaults", {"asset_path": "/Game/Ripper/BP_Lift"}, True, True),
        ("blueprint_cmd", "graph.trace_exec", {"asset_path": "/Game/Ripper/BP_Dumbwaiter"}, True, True),
        ("blueprint_cmd", "blueprint.list_settable_defaults", {"asset_path": "/Game/Ripper/BP_Dumbwaiter"}, True, True),
        ("level_cmd", "level.list_actor_classes", {"category": "Blueprint"}, True, True),
        ("blueprint_cmd", "graph.trace_exec", {"asset_path": "/Game/Ripper/BP_Lift"}, True, True),
        ("blueprint_cmd", "blueprint.set_class_defaults", {"items": [{"target": "/Game/Ripper/BP_Lift", "properties": {"OpenSeq": "DoorOpenSeq"}, "expected_fingerprint": {"dirty_epoch": 42}}]}, False, False),
        ("blueprint_cmd", "blueprint.save", {"items": [{"target": "/Game/Ripper/BP_Lift", "expected_fingerprint": {"dirty_epoch": 42}}]}, False, False),
    ]

    for tool_name, command, params, use_cache, parallel in steps:
        conn.record_tool_invocation(tool_name, command, parallel=parallel)
        if use_cache:
            conn.send_command_cached(command, params, ttl=300)
        else:
            conn.send_command(command, params)

    return SimpleNamespace(**conn.get_call_metrics())


def test_server_exports_call_count_metrics(monkeypatch):
    """Server compatibility helper should expose current connection telemetry."""
    expected = {
        "logical_tool_calls": 3,
        "tcp_calls": 2,
        "python_cache_hits": 1,
        "parallel_sequential_ratio": 0.5,
        "repeat_read_ratio": 0.25,
    }
    monkeypatch.setattr(server._connection, "get_call_metrics", lambda: expected)

    payload = json.loads(server.get_call_count_metrics())

    assert payload == expected


def test_ripper_scenario_call_budget():
    """The canonical scenario should stay within the target post-fix budget."""
    result = run_canonical_ripper_scenario()

    assert result.logical_tool_calls <= 10
    assert result.tcp_calls <= 10
    assert result.python_cache_hits >= 1
    assert result.parallel_sequential_ratio >= 0.70
    assert result.repeat_read_ratio < 0.15


def test_toolkit_prompts_require_prefetched_state_and_expected_fingerprint():
    """Task-launch prompts should require prefetched state and stale-write guards."""
    files = [
        _WORKSPACE_ROOT / "cortex-toolkit/agents/blueprint-developer.md",
        _WORKSPACE_ROOT / "cortex-toolkit/agents/level-designer.md",
        _WORKSPACE_ROOT / "cortex-toolkit/agents/material-developer.md",
        _WORKSPACE_ROOT / "cortex-toolkit/skills/cortex-blueprint/SKILL.md",
        _WORKSPACE_ROOT / "cortex-toolkit/skills/cortex-level/SKILL.md",
        _WORKSPACE_ROOT / "cortex-toolkit/skills/cortex-material/SKILL.md",
    ]

    for path in files:
        text = path.read_text(encoding="utf-8")
        assert "prefetched_state" in text, f"{path.name} is missing prefetched_state guidance"
        assert "expected_fingerprint" in text, f"{path.name} is missing expected_fingerprint guidance"

    for path in files[:3]:
        text = path.read_text(encoding="utf-8").lower()
        assert "parallel" in text, f"{path.name} is missing the parallel read contract"


def test_call_count_docs_capture_shipped_surface():
    """Implementation and system docs should describe the shipped reduction surface."""
    files = [
        _WORKSPACE_ROOT / "docs/plans/2026-04-13-call-count-reduction-impl.md",
        _WORKSPACE_ROOT / "docs/verification/call-count-reduction-test-verification.md",
        _WORKSPACE_ROOT / "docs/systems/INDEX.md",
        _WORKSPACE_ROOT / "docs/systems/cortex-core.md",
        _WORKSPACE_ROOT / "docs/systems/cortex-graph.md",
        _WORKSPACE_ROOT / "docs/systems/cortex-blueprint.md",
        _WORKSPACE_ROOT / "docs/systems/cortex-level.md",
        _WORKSPACE_ROOT / "docs/systems/python-mcp.md",
        _WORKSPACE_ROOT / "docs/requests/core/2026-04-09-call-count-reduction.md",
        _WORKSPACE_ROOT / "docs/requests/INDEX.md",
    ]

    for path in files:
        assert path.exists(), f"missing documentation file: {path.name}"

    impl_doc = files[0].read_text(encoding="utf-8")
    verification_doc = files[1].read_text(encoding="utf-8")
    python_mcp_doc = files[7].read_text(encoding="utf-8")

    assert "asset_fingerprint" in impl_doc
    assert "repeat_read_ratio" in impl_doc
    assert "logical_tool_calls" in verification_doc
    assert "parallel_sequential_ratio" in verification_doc
    assert "prefetched_state" in python_mcp_doc
