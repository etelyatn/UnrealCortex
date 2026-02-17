"""TCP client for communicating with the UnrealCortex plugin's TCP server."""

import os
import pathlib
import socket
import json
import logging
import threading
import time
import uuid

from .cache import ResponseCache

logger = logging.getLogger(__name__)

_CONNECT_TIMEOUT = 5.0
_RECV_TIMEOUT = 60.0
_RECONNECT_DELAY = 0.5
_DEFAULT_PORT = 8742
_PORT_FILENAME = "CortexPort.txt"


def _discover_port() -> int | None:
    """Discover the Cortex TCP port from the port file written by CortexCore.

    Searches for CortexPort.txt in the Saved/ directory of the project root.
    The project root is determined by:
    1. CORTEX_PROJECT_DIR environment variable (if set)
    2. Searching parent directories of this file for a .uproject file

    Returns:
        The port number from the file, or None if not found.
    """
    # Try env var first
    project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    if project_dir:
        port_file = pathlib.Path(project_dir) / "Saved" / _PORT_FILENAME
        if port_file.exists():
            try:
                port = int(port_file.read_text().strip())
                logger.info("Discovered port %d from %s", port, port_file)
                return port
            except (ValueError, OSError) as e:
                logger.warning("Failed to read port file %s: %s", port_file, e)
        else:
            logger.debug("Port file not found at %s", port_file)
        return None

    # Search parent directories for a .uproject file
    current = pathlib.Path(__file__).resolve().parent
    for _ in range(20):  # Safety limit
        uproject_files = list(current.glob("*.uproject"))
        if uproject_files:
            port_file = current / "Saved" / _PORT_FILENAME
            if port_file.exists():
                try:
                    port = int(port_file.read_text().strip())
                    logger.info("Discovered port %d from %s", port, port_file)
                    return port
                except (ValueError, OSError) as e:
                    logger.warning("Failed to read port file %s: %s", port_file, e)
            else:
                logger.debug("Port file not found at %s", port_file)
            return None
        parent = current.parent
        if parent == current:
            break
        current = parent

    logger.debug("No .uproject found in parent directories")
    return None


class UEConnection:
    """Manages TCP connection to the UnrealCortex plugin."""

    def __init__(self, host: str = "127.0.0.1", port: int | None = None):
        self.host = host
        if port is not None:
            self.port = port
        else:
            # Try port file discovery, then fallback to default
            discovered = _discover_port()
            self.port = discovered if discovered is not None else _DEFAULT_PORT
        self._socket: socket.socket | None = None
        self._cache = ResponseCache()
        self._recv_buffer = b""
        self._socket_lock = threading.Lock()

    @property
    def connected(self) -> bool:
        return self._socket is not None

    def connect(self) -> None:
        """Connect to the UE plugin TCP server. Raises ConnectionError if unavailable."""
        if self._socket is not None:
            return

        logger.debug("Connecting to Unreal Editor at %s:%d", self.host, self.port)
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(_CONNECT_TIMEOUT)
            sock.connect((self.host, self.port))
            sock.settimeout(_RECV_TIMEOUT)
            self._socket = sock
            logger.info("Connected to Unreal Editor at %s:%d", self.host, self.port)
        except (ConnectionRefusedError, TimeoutError, OSError) as e:
            self._socket = None
            raise ConnectionError(
                f"Cannot connect to Unreal Editor at {self.host}:{self.port}. "
                f"Is the editor running with UnrealCortex plugin enabled? Error: {e}"
            ) from e

    def disconnect(self) -> None:
        """Close the TCP connection."""
        if self._socket:
            try:
                self._socket.close()
            except OSError:
                pass
            self._socket = None
        self._recv_buffer = b""

    def send_command(
        self,
        command: str,
        params: dict | None = None,
        timeout: float | None = None,
    ) -> dict:
        """Send a command to the UE plugin and return the response.

        On connection failure, automatically retries once after a brief delay.
        Timeout applies to the total operation (including deferred ack + final).
        Raises ConnectionError if not connected after retry.
        Raises RuntimeError if the command fails.

        Args:
            command: Command name
            params: Optional command parameters
            timeout: Optional timeout in seconds for this command (overrides default)
        """
        last_error: Exception | None = None

        for attempt in range(2):
            try:
                with self._socket_lock:
                    self.connect()
                    return self._send_and_receive(command, params, timeout=timeout)
            except ConnectionError as e:
                last_error = e
                self.disconnect()
                if attempt == 0:
                    logger.debug(
                        "Connection lost during '%s', reconnecting in %.1fs",
                        command,
                        _RECONNECT_DELAY,
                    )
                    # Connection dropped - clear all caches (editor may have restarted)
                    cleared = self._cache.invalidate(None)
                    if cleared > 0:
                        logger.info(
                            "Cleared %d cache entries on reconnect", cleared
                        )
                    time.sleep(_RECONNECT_DELAY)

        raise last_error

    def send_command_cached(
        self,
        command: str,
        params: dict | None = None,
        ttl: float = 300.0,
        timeout: float | None = None,
    ) -> dict:
        """Send a command with response caching.

        Returns cached response if available and not expired.
        Otherwise sends the command and caches the successful response.
        """
        key = ResponseCache.make_key(command, params)
        cached = self._cache.get(key)
        if cached is not None:
            return cached

        response = self.send_command(command, params, timeout=timeout)
        self._cache.set(key, response, ttl)
        return response

    def invalidate_cache(self, pattern: str | None) -> int:
        """Invalidate cache entries. None clears all."""
        return self._cache.invalidate(pattern)

    def _read_response_line(self, deadline: float) -> str:
        """Read one newline-delimited JSON response line from the socket."""
        while b"\n" not in self._recv_buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ConnectionError("Timed out waiting for Unreal Editor response")

            self._socket.settimeout(remaining)
            chunk = self._socket.recv(65536)
            if not chunk:
                self.disconnect()
                raise ConnectionError("Connection closed by Unreal Editor")
            self._recv_buffer += chunk

        line, self._recv_buffer = self._recv_buffer.split(b"\n", 1)
        return line.decode("utf-8")

    def _send_and_receive(
        self,
        command: str,
        params: dict | None = None,
        timeout: float | None = None,
    ) -> dict:
        """Send a command and read the response. Internal method, no retry logic.

        Args:
            command: Command name
            params: Optional parameters
            timeout: Optional timeout in seconds (overrides default recv timeout)
        """
        request_id = uuid.uuid4().hex[:8]
        request = json.dumps(
            {"id": request_id, "command": command, "params": params or {}}
        ) + "\n"
        start = time.monotonic()
        timeout_seconds = timeout if timeout is not None else _RECV_TIMEOUT
        deadline = start + timeout_seconds
        try:
            self._socket.sendall(request.encode("utf-8"))
            response = json.loads(self._read_response_line(deadline))

            # Deferred protocol: first line is ack; final line contains command result.
            if response.get("status") == "deferred":
                while True:
                    response = json.loads(self._read_response_line(deadline))
                    if response.get("id") and response.get("id") != request_id:
                        logger.warning(
                            "Ignoring mismatched deferred response id '%s' for '%s'",
                            response.get("id"),
                            command,
                        )
                        continue
                    if response.get("status") in ("", None, "complete"):
                        break

            elapsed = time.monotonic() - start
            logger.debug("Command '%s' completed in %.3fs", command, elapsed)

            if not response.get("success"):
                error = response.get("error", {})
                raise RuntimeError(
                    f"UE command '{command}' failed: {error.get('message', 'Unknown error')} "
                    f"(code: {error.get('code', 'UNKNOWN')})"
                )

            return response

        except (BrokenPipeError, ConnectionResetError, OSError, json.JSONDecodeError) as e:
            self.disconnect()
            raise ConnectionError(f"Lost connection to Unreal Editor: {e}") from e
