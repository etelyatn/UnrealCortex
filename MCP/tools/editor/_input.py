"""Internal editor input helpers (not auto-registered as MCP tools)."""

from cortex_mcp.tcp_client import UEConnection


def inject_key(connection: UEConnection, key: str, action: str = "tap", duration_ms: int = 50) -> dict:
    params = {"key": key, "action": action, "duration_ms": duration_ms}
    return connection.send_command("editor.inject_key", params)


def inject_mouse(connection: UEConnection, button: str, action: str = "click") -> dict:
    params = {"button": button, "action": action}
    return connection.send_command("editor.inject_mouse", params)


def inject_input_action(connection: UEConnection, action_name: str, value: float = 1.0) -> dict:
    params = {"action_name": action_name, "value": value}
    return connection.send_command("editor.inject_input_action", params)


def inject_sequence(connection: UEConnection, steps: list[dict], timeout: float = 60.0) -> dict:
    return connection.send_command("editor.inject_input_sequence", {"steps": steps}, timeout=timeout)
