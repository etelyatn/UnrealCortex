"""End-to-end tests for editor domain TCP commands.

Requires a running Unreal Editor instance with the UnrealCortex plugin enabled.
"""

import pathlib

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


# ================================================================
# Viewport / Screenshot / Logs Operations (non-PIE, fast)
# ================================================================


@pytest.mark.e2e
def test_get_viewport_info(editor_connection):
    result = editor_connection.send_command("editor.get_viewport_info")
    assert result["success"] is True
    data = result["data"]
    assert "resolution" in data
    cam = data["camera_location"]
    assert "x" in cam and "y" in cam and "z" in cam
    assert "view_mode" in data


@pytest.mark.e2e
def test_set_viewport_camera_and_readback(editor_connection):
    editor_connection.send_command("editor.set_viewport_camera", {
        "location": {"x": 500.0, "y": 300.0, "z": 200.0},
    })
    result = editor_connection.send_command("editor.get_viewport_info")
    assert result["success"] is True
    cam = result["data"]["camera_location"]
    assert cam["x"] == pytest.approx(500.0, abs=0.01)
    assert cam["y"] == pytest.approx(300.0, abs=0.01)
    assert cam["z"] == pytest.approx(200.0, abs=0.01)


@pytest.mark.e2e
def test_capture_screenshot(editor_connection):
    screenshot_path = None
    try:
        result = editor_connection.send_command("editor.capture_screenshot", {})
        assert result["success"] is True
        data = result["data"]
        assert "path" in data
        assert data["width"] > 0
        assert data["height"] > 0
        assert data["file_size_bytes"] > 0
        screenshot_path = data["path"]
        assert pathlib.Path(screenshot_path).exists()
    finally:
        if screenshot_path:
            try:
                pathlib.Path(screenshot_path).unlink()
            except OSError:
                pass


@pytest.mark.e2e
def test_set_viewport_mode_cycle(editor_connection):
    try:
        for mode in ("unlit", "wireframe"):
            editor_connection.send_command("editor.set_viewport_mode", {
                "mode": mode,
            })
            result = editor_connection.send_command("editor.get_viewport_info")
            assert result["success"] is True
            assert result["data"]["view_mode"].lower() == mode
    finally:
        try:
            editor_connection.send_command("editor.set_viewport_mode", {
                "mode": "lit",
            })
        except Exception:
            pass


@pytest.mark.e2e
def test_get_recent_logs(editor_connection):
    result = editor_connection.send_command("editor.get_recent_logs", {
        "since_seconds": 30.0,
        "severity": "log",
    })
    assert result["success"] is True
    data = result["data"]
    assert isinstance(data["entries"], list)
    assert isinstance(data["cursor"], (int, float))


# ================================================================
# Editor Error Cases
# ================================================================


@pytest.mark.e2e
def test_set_viewport_mode_invalid(editor_connection):
    with pytest.raises(RuntimeError):
        editor_connection.send_command("editor.set_viewport_mode", {
            "mode": "nonexistent_mode_12345",
        })


@pytest.mark.e2e
def test_focus_actor_not_found(editor_connection):
    with pytest.raises(RuntimeError):
        editor_connection.send_command("editor.focus_actor", {
            "actor_path": "/Game/NonExistent/Actor_12345",
        })
