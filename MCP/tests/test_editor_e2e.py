"""End-to-end tests for editor domain TCP commands.

Requires a running Unreal Editor instance with the UnrealCortex plugin enabled.
"""

import pytest

from cortex_mcp.tcp_client import UEConnection


@pytest.fixture(scope="module")
def editor_connection():
    """Create a connection to the running editor, or skip when unavailable."""
    conn = UEConnection()
    try:
        conn.send_command("get_status")
    except Exception as exc:
        conn.disconnect()
        pytest.skip(f"No running Unreal Editor available for editor E2E tests: {exc}")
    yield conn
    conn.disconnect()


@pytest.mark.e2e
def test_get_pie_state_when_stopped(editor_connection):
    result = editor_connection.send_command("editor.get_pie_state")
    assert result["success"] is True
    assert "state" in result["data"]


@pytest.mark.e2e
def test_get_editor_state(editor_connection):
    result = editor_connection.send_command("editor.get_editor_state")
    assert result["success"] is True
    assert "project_name" in result["data"]
    assert "pie_state" in result["data"]


@pytest.mark.e2e
def test_start_stop_pie_lifecycle(editor_connection):
    start = editor_connection.send_command(
        "editor.start_pie", {"mode": "selected_viewport"}, timeout=60.0
    )
    assert start["success"] is True
    assert start["data"]["state"] == "playing"

    state = editor_connection.send_command("editor.get_pie_state")
    assert state["success"] is True
    assert state["data"]["state"] in ("playing", "paused")

    stop = editor_connection.send_command("editor.stop_pie", {}, timeout=30.0)
    assert stop["success"] is True

    final_state = editor_connection.send_command("editor.get_pie_state")
    assert final_state["success"] is True
    assert final_state["data"]["state"] == "stopped"


@pytest.mark.e2e
def test_pause_resume(editor_connection):
    start = editor_connection.send_command(
        "editor.start_pie", {"mode": "selected_viewport"}, timeout=60.0
    )
    assert start["success"] is True

    try:
        pause = editor_connection.send_command("editor.pause_pie")
        assert pause["success"] is True

        paused_state = editor_connection.send_command("editor.get_pie_state")
        assert paused_state["success"] is True
        assert paused_state["data"]["state"] == "paused"

        resume = editor_connection.send_command("editor.resume_pie")
        assert resume["success"] is True

        resumed_state = editor_connection.send_command("editor.get_pie_state")
        assert resumed_state["success"] is True
        assert resumed_state["data"]["state"] == "playing"
    finally:
        try:
            editor_connection.send_command("editor.stop_pie", {}, timeout=30.0)
        except Exception:
            pass


@pytest.mark.e2e
def test_start_pie_when_already_active(editor_connection):
    first_start = editor_connection.send_command(
        "editor.start_pie", {"mode": "selected_viewport"}, timeout=60.0
    )
    assert first_start["success"] is True

    try:
        with pytest.raises(RuntimeError) as exc_info:
            editor_connection.send_command(
                "editor.start_pie", {"mode": "selected_viewport"}, timeout=10.0
            )
        message = str(exc_info.value)
        assert "PIE_ALREADY_ACTIVE" in message
    finally:
        try:
            editor_connection.send_command("editor.stop_pie", {}, timeout=30.0)
        except Exception:
            pass
