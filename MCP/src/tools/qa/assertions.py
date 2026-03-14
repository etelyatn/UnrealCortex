"""QA assertion tools."""

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection


def register_qa_assertion_tools(mcp, connection: UEConnection):
    """Register QA assertion tools."""

    @mcp.tool()
    def assert_game_state(
        type: str,
        actor: str = "",
        property: str = "",
        value: bool | float | str | int | None = None,
        expected: bool = True,
        message: str = "",
    ) -> str:
        try:
            params: dict = {"type": type, "expected": expected}
            if actor:
                params["actor"] = actor
            if property:
                params["property"] = property
            if value is not None:
                params["value"] = value
            if message:
                params["message"] = message

            response = connection.send_command("qa.assert_state", params)
            data = response.get("data", {})

            if not data.get("passed", False):
                try:
                    connection.send_command(
                        "editor.capture_screenshot",
                        {"filename": "qa_assertion_failed.png"},
                        timeout=60.0,
                    )
                except (RuntimeError, ConnectionError):
                    pass

            return format_response(data, "assert_game_state")
        except (RuntimeError, ConnectionError) as e:
            return f"Error: {e}"
