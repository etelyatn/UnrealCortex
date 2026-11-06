"""One public result representation for every registered MCP tool.

Every public registration (domain routers, composites, standalone tools, Reflect, import
queues and the optional Gen composite) goes through :class:`ToolOutputRegistrar`. The
facade owns the public encoding:

* ``text`` (default): exactly one compact JSON text block, no ``structuredContent``.
* ``structured``: the same compact text plus the decoded object under ``structuredContent``.

Explicit ``CallToolResult``/image content is passed through untouched, and text that is not
JSON stays an opaque text block with its original meaning. Response-size telemetry is attributed
to the command routed *inside the current invocation* (an invocation-local scope, so concurrent
async invocations stay isolated and a refusal that routes nothing reports the explicit unknown
state). This is a registration adapter, not a general plugin framework and not a semantic
truncator: domain owners keep their meaning, guards and outcome envelopes.
"""

from __future__ import annotations

import functools
import inspect
import json
from typing import Any, Callable

from mcp.types import CallToolResult, TextContent

from .response import compact_json
from .tcp_client import INVOCATION_COMMAND

_TEXT_MODE = "text"
_STRUCTURED_MODE = "structured"
RESPONSE_MODES = (_TEXT_MODE, _STRUCTURED_MODE)


class ToolOutputRegistrar:
    """Register public tools behind a single result-representation owner."""

    def __init__(
        self,
        mcp_server,
        connection,
        response_mode: str = _TEXT_MODE,
        allowed_tools: Any = None,
        metrics: bool = False,
    ) -> None:
        if response_mode not in RESPONSE_MODES:
            raise ValueError(
                f"Invalid response mode {response_mode!r}; choose one of {', '.join(RESPONSE_MODES)}."
            )
        self._server = mcp_server
        self._connection = connection
        self._response_mode = response_mode
        # None exposes every current registration; an explicit set is a real allowlist.
        self._allowed_tools = None if allowed_tools is None else frozenset(allowed_tools)
        self._metrics = bool(metrics)

    @property
    def _tool_manager(self):
        """Delegate the strict-router argument-model customization to the real server."""
        return getattr(self._server, "_tool_manager", None)

    def tool(self, name: str | None = None, description: str | None = None, **kwargs):
        """Registration decorator mirroring ``FastMCP.tool`` for the public surface."""

        def decorator(fn: Callable[..., Any]) -> Callable[..., Any]:
            tool_name = name or getattr(fn, "__name__", None)
            if self._allowed_tools is not None and tool_name not in self._allowed_tools:
                # Excluded tools are not registered and are not wrapped.
                return fn
            # The adapter owns the representation: never let the SDK wrap a string return
            # in an automatic output schema or a {"result": ...} object.
            kwargs["structured_output"] = False
            self._server.tool(name=tool_name, description=description, **kwargs)(
                self._wrap(fn, tool_name)
            )
            # Mirror FastMCP: the module-local name keeps the raw implementation so
            # direct internal calls keep their existing string contract.
            return fn

        return decorator

    def _wrap(self, fn: Callable[..., Any], tool_name: str) -> Callable[..., Any]:
        if inspect.iscoroutinefunction(fn):

            @functools.wraps(fn)
            async def async_wrapper(*args, **kwargs) -> CallToolResult:
                token = INVOCATION_COMMAND.set("")
                try:
                    value = await fn(*args, **kwargs)
                    routed_command = INVOCATION_COMMAND.get()
                finally:
                    INVOCATION_COMMAND.reset(token)
                return self._build_result(tool_name, value, routed_command)

            return async_wrapper

        @functools.wraps(fn)
        def sync_wrapper(*args, **kwargs) -> CallToolResult:
            token = INVOCATION_COMMAND.set("")
            try:
                value = fn(*args, **kwargs)
                routed_command = INVOCATION_COMMAND.get()
            finally:
                INVOCATION_COMMAND.reset(token)
            return self._build_result(tool_name, value, routed_command)

        return sync_wrapper

    def _build_result(self, tool_name: str, value: Any, routed_command: str) -> CallToolResult:
        if isinstance(value, CallToolResult):
            return self._with_metrics(tool_name, value, self._text_of(value), routed_command)

        data, text, decoded = self._normalize(value)
        structured = (
            data if (decoded and isinstance(data, dict) and self._response_mode == _STRUCTURED_MODE) else None
        )
        result = CallToolResult(
            content=[TextContent(type="text", text=text)],
            structuredContent=structured,
        )
        return self._with_metrics(tool_name, result, text, routed_command)

    @staticmethod
    def _normalize(value: Any) -> tuple[Any, str, bool]:
        """Return ``(data, emitted_text, decoded)`` for one tool return value."""
        if isinstance(value, str):
            try:
                data = json.loads(value)
            except (ValueError, TypeError):
                # Non-JSON text failures stay opaque: never an empty object, never inferred codes.
                return None, value, False
            return data, compact_json(data), True
        if isinstance(value, (dict, list, bool, int, float)) or value is None:
            return value, compact_json(value), True
        # Defensive: unexpected non-JSON returns are emitted as opaque text.
        return None, str(value), False

    @staticmethod
    def _text_of(result: CallToolResult) -> str:
        for block in result.content:
            text = getattr(block, "text", None)
            if isinstance(text, str):
                return text
        return ""

    def _with_metrics(
        self, tool_name: str, result: CallToolResult, text: str, routed_command: str
    ) -> CallToolResult:
        if not self._metrics:
            return result

        json_chars = len(text)
        utf8_bytes = len(text.encode("utf-8"))
        # Telemetry excludes its own metadata and is reported once per public return.
        meta = dict(result.meta or {})
        cortex = dict(meta.get("cortex") or {})
        cortex["json_chars"] = json_chars
        cortex["utf8_bytes"] = utf8_bytes
        meta["cortex"] = cortex
        result.meta = meta

        record = getattr(self._connection, "record_response_size", None)
        if callable(record):
            # The command routed inside this invocation, or "" for the explicit unknown/refused
            # state; the size is attributed to nothing rather than to call history.
            record(tool_name, routed_command, json_chars, utf8_bytes)
        return result
