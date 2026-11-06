"""Unit tests for Blueprint migration MCP tool wrappers."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock

TOOLS_DIR = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from blueprint.migration import register_blueprint_migration_tools


class MockMCP:
    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


def _register_tools(connection):
    mcp = MockMCP()
    register_blueprint_migration_tools(mcp, connection)
    return mcp.tools


def test_remove_scs_component_forwards_acknowledgment_and_force():
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"removed_component": "OpenSeq"}}

    tools = _register_tools(connection)
    result = tools["remove_scs_component"](
        asset_path="/Game/BP_Test",
        component_name="OpenSeq",
        compile=False,
        acknowledged_losses=["Payload"],
        force=True,
    )

    parsed = json.loads(result)
    assert parsed["removed_component"] == "OpenSeq"
    connection.send_command.assert_called_once_with(
        "blueprint.remove_scs_component",
        {
            "asset_path": "/Game/BP_Test",
            "component_name": "OpenSeq",
            "compile": False,
            "acknowledged_losses": ["Payload"],
            "force": True,
        },
    )


def test_rename_scs_component_forwards_compile_flag():
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"new_name": "LegacyOpenSeq"}}

    tools = _register_tools(connection)
    result = tools["rename_scs_component"](
        asset_path="/Game/BP_Test",
        old_name="OpenSeq",
        new_name="LegacyOpenSeq",
        compile=False,
    )

    parsed = json.loads(result)
    assert parsed["new_name"] == "LegacyOpenSeq"
    connection.send_command.assert_called_once_with(
        "blueprint.rename_scs_component",
        {
            "asset_path": "/Game/BP_Test",
            "old_name": "OpenSeq",
            "new_name": "LegacyOpenSeq",
            "compile": False,
        },
    )


# --- cleanup_blueprint_migration forwards the guarded-cleanup fields ---

from blueprint.analysis import register_blueprint_analysis_tools


def _cleanup_tools(connection):
    mcp = MockMCP()
    register_blueprint_analysis_tools(mcp, connection)
    return mcp.tools


def test_cleanup_forwards_guarded_fields_and_never_prunes_a_dry_run():
    connection = MagicMock()
    connection.send_command.return_value = {
        "data": {
            "apply_status": "dry_run",
            "referencing_nodes": [{"variable": "bPowerOn", "node_guid": "GUID-1"}],
            "save_game_findings": [],
            "reparent_plan": {"collisions": []},
            "cdo_expectations": {"bPowerOn": "True"},
        }
    }

    tools = _cleanup_tools(connection)
    result = json.loads(
        tools["cleanup_blueprint_migration"](
            asset_path="/Game/BP_Test",
            new_parent_class="/Script/Game.AEnemyBase",
            remove_variables=["bPowerOn"],
            migrated_overrides=["BeginPlay"],
            dry_run=True,
            save=False,
            expected_fingerprint={"migration_state_hash": "abc"},
            remove_referencing_nodes=True,
            acknowledged_collisions=["variable:bPowerOn"],
            acknowledged_losses=["save_game:bOpen"],
        )
    )

    assert result["apply_status"] == "dry_run"
    commands = [c.args[0] for c in connection.send_command.call_args_list]
    assert commands == ["blueprint.cleanup_migration"]
    params = connection.send_command.call_args_list[0].args[1]
    assert params == {
        "asset_path": "/Game/BP_Test",
        "compile": False,
        "save": False,
        "new_parent_class": "/Script/Game.AEnemyBase",
        "remove_variables": ["bPowerOn"],
        "dry_run": True,
        "remove_referencing_nodes": True,
        "expected_fingerprint": {"migration_state_hash": "abc"},
        "acknowledged_collisions": ["variable:bPowerOn"],
        "acknowledged_losses": ["save_game:bOpen"],
    }


def test_cleanup_chains_stage_cdo_expectations_into_the_finalize_call():
    connection = MagicMock()
    connection.send_command.side_effect = [
        {"data": {"cdo_expectations": {"bPowerOn": "True"}, "adopted_by_engine": ["bPowerOn"]}},
        {"data": {"saved": True, "is_dirty": False}},
    ]

    tools = _cleanup_tools(connection)
    result = json.loads(
        tools["cleanup_blueprint_migration"](
            asset_path="/Game/BP_Test",
            migrated_overrides=["Unknown"],
        )
    )

    finalize = connection.send_command.call_args_list[-1].args[1]
    assert finalize["expected_cdo_values"] == {"bPowerOn": "True"}
    assert finalize["save"] is True and finalize["compile"] is True
    assert result["adopted_by_engine"] == ["bPowerOn"]


def test_cleanup_dry_run_with_save_refuses_without_a_command():
    connection = MagicMock()
    tools = _cleanup_tools(connection)

    result = json.loads(tools["cleanup_blueprint_migration"](asset_path="/Game/BP_Test", dry_run=True))

    assert "error" in result
    connection.send_command.assert_not_called()
