"""E2E tests for Blueprint migration MCP tools."""

import pytest
import uuid


def _uniq(prefix: str) -> str:
    return f"{prefix}_{uuid.uuid4().hex[:8]}"


@pytest.mark.e2e
def test_blueprint_migration_tool_flow(tcp_connection):
    base_name = _uniq("BP_MigrationTool")
    source_name = f"{base_name}_Source"
    copy_name = f"{base_name}_Copy"
    renamed_name = f"{base_name}_Renamed"
    path = "/Game/Temp/CortexMCPTest"

    source_path = f"{path}/{source_name}"
    copy_path = f"{path}/{copy_name}"
    renamed_path = f"{path}/{renamed_name}"

    created_paths = []

    try:
        create_resp = tcp_connection.send_command("bp.create", {
            "name": source_name,
            "path": path,
            "type": "Actor",
        })
        assert create_resp.get("success", False)
        created_paths.append(source_path)

        dup_resp = tcp_connection.send_command("bp.duplicate", {
            "asset_path": source_path,
            "new_name": copy_name,
            "new_path": path,
        })
        assert dup_resp.get("success", False)
        created_paths.append(copy_path)

        rename_resp = tcp_connection.send_command("bp.rename", {
            "source_path": copy_path,
            "dest_path": renamed_path,
        })
        assert rename_resp.get("success", False)

        # copy_path becomes redirector after rename; keep renamed path for cleanup
        created_paths = [source_path, renamed_path]

        compare_resp = tcp_connection.send_command("bp.compare_blueprints", {
            "source_path": source_path,
            "target_path": renamed_path,
        })
        assert compare_resp.get("success", False)
        assert "match" in compare_resp.get("data", {})

        fixup_resp = tcp_connection.send_command("bp.fixup_redirectors", {
            "path": path,
            "recursive": True,
        })
        assert fixup_resp.get("success", False)

    finally:
        for asset_path in reversed(created_paths):
            try:
                tcp_connection.send_command("bp.delete", {
                    "asset_path": asset_path,
                    "force": True,
                })
            except Exception:
                pass
