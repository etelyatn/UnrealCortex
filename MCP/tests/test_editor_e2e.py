"""End-to-end tests for editor domain TCP commands.

Requires a running Unreal Editor instance with the UnrealCortex plugin enabled.
"""

from pathlib import Path
import time

import pytest

from cortex_mcp.tcp_client import UECommandError, UEConnection


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


def _stop_pie_if_running(conn):
    try:
        state = conn.send_command("editor.get_pie_state")
        if state.get("data", {}).get("state") != "stopped":
            conn.send_command("editor.stop_pie", {}, timeout=30.0)
    except Exception:
        pass


def _send_python_or_skip(editor_connection, params):
    try:
        return editor_connection.send_command("editor.run_python", params, timeout=30.0)
    except UECommandError as exc:
        if exc.code == "UNSUPPORTED_COMMAND":
            pytest.skip("PythonScriptPlugin unavailable")
        raise


@pytest.fixture(autouse=True)
def ensure_pie_stopped(editor_connection):
    _stop_pie_if_running(editor_connection)
    yield
    _stop_pie_if_running(editor_connection)


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
    first_start = None
    last_exc = None
    for _ in range(2):
        try:
            first_start = editor_connection.send_command(
                "editor.start_pie", {"mode": "selected_viewport"}, timeout=60.0
            )
            break
        except RuntimeError as exc:
            last_exc = exc
            if "PIE_TERMINATED" not in str(exc):
                raise
            time.sleep(1.0)

    if first_start is None:
        raise last_exc
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
        assert Path(screenshot_path).exists()
    finally:
        if screenshot_path:
            try:
                Path(screenshot_path).unlink()
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


@pytest.mark.e2e
def test_run_python_basic_execution(editor_connection):
    result = _send_python_or_skip(editor_connection, {"code": "print('cortex e2e python')"})
    assert result["success"] is True
    data = result["data"]
    assert data["ok"] is True
    assert data["output_truncated"] is False
    assert isinstance(data["output"], list)
    assert any("cortex e2e python" in entry.get("text", "") for entry in data["output"])


@pytest.mark.e2e
def test_run_python_next_tick(editor_connection):
    result = _send_python_or_skip(
        editor_connection,
        {"code": "print('cortex e2e next tick')", "run_next_tick": True},
    )
    assert result["success"] is True
    assert result["data"]["ok"] is True


@pytest.mark.e2e
def test_run_python_error_and_missing_code(editor_connection):
    with pytest.raises(UECommandError) as missing:
        editor_connection.send_command("editor.run_python", {}, timeout=10.0)
    assert missing.value.code == "INVALID_FIELD"

    try:
        editor_connection.send_command(
            "editor.run_python",
            {"code": "raise RuntimeError('cortex e2e expected failure')"},
            timeout=30.0,
        )
    except UECommandError as exc:
        if exc.code == "UNSUPPORTED_COMMAND":
            pytest.skip("PythonScriptPlugin unavailable")
        assert exc.code == "INVALID_OPERATION"
        assert "result" in exc.details
        assert "output" in exc.details
    else:
        pytest.fail("Python error should raise UECommandError")


@pytest.mark.e2e
def test_cvar_get_set_list_with_restore(editor_connection):
    original = editor_connection.send_command("editor.get_cvar", {"name": "t.MaxFPS"}, timeout=10.0)
    assert original["success"] is True
    original_value = original["data"]["value"]
    new_value = "29" if original_value != "29" else "31"

    try:
        changed = editor_connection.send_command(
            "editor.set_cvar",
            {"name": "t.MaxFPS", "value": new_value},
            timeout=10.0,
        )
        assert changed["success"] is True
        assert changed["data"]["old_value"] == original_value
        assert changed["data"]["value"] == new_value

        listed = editor_connection.send_command(
            "editor.list_cvars",
            {"pattern": "t.", "limit": 10},
            timeout=10.0,
        )
        assert listed["success"] is True
        assert "variables" in listed["data"]
        assert "commands" in listed["data"]
        assert "cvars" not in listed["data"]
        assert listed["data"]["returned_count"] <= 10
    finally:
        editor_connection.send_command(
            "editor.set_cvar",
            {"name": "t.MaxFPS", "value": original_value},
            timeout=10.0,
        )


@pytest.mark.e2e
def test_unknown_cvar_error(editor_connection):
    with pytest.raises(UECommandError) as exc_info:
        editor_connection.send_command("editor.get_cvar", {"name": "cortex.DoesNotExist"}, timeout=10.0)
    assert exc_info.value.code == "SYMBOL_NOT_FOUND"


@pytest.mark.anyio
@pytest.mark.e2e
async def test_editor_router_tool(mcp_client):
    result = await mcp_client.call_tool("editor_cmd", {"command": "get_editor_state", "params": {}})
    payload = __import__("json").loads(result.content[0].text)
    assert "project_name" in payload
