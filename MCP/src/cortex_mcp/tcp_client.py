"""TCP client for communicating with the UnrealCortex plugin's TCP server."""

import ctypes
import dataclasses
import os
import pathlib
import platform
import socket
import json
import logging
import threading
import time
import uuid

from .cache import ResponseCache

if platform.system() == "Windows":
    from ctypes import wintypes

logger = logging.getLogger(__name__)

_CONNECT_TIMEOUT = 5.0
_RECV_TIMEOUT = 60.0
_RECONNECT_DELAY = 0.5
_DEFAULT_PORT = 8742


@dataclasses.dataclass(frozen=True)
class EditorConnection:
    """Metadata for a discovered Unreal Editor instance."""

    port: int
    pid: int
    started_at: str
    port_file: pathlib.Path
    project: str = ""


_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def _is_editor_alive(pid: int) -> bool:
    """Check if PID is a live Unreal Editor process.

    Windows: OpenProcess + QueryFullProcessImageNameW to verify executable.
    Unix: os.kill(pid, 0) signal check.
    """
    if platform.system() == "Windows":
        handle = ctypes.windll.kernel32.OpenProcess(
            _PROCESS_QUERY_LIMITED_INFORMATION, False, pid
        )
        if not handle:
            error = ctypes.GetLastError()
            if error == 5:  # ERROR_ACCESS_DENIED - process exists
                return True
            return False
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(1024)
            ok = ctypes.windll.kernel32.QueryFullProcessImageNameW(
                handle, 0, buf, ctypes.byref(size)
            )
            if not ok:
                return False
            return "UnrealEditor" in buf.value
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    else:
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True


def _pid_from_port_filename(port_file: pathlib.Path) -> int:
    """Extract PID from a CortexPort-{PID}.txt filename. Returns 0 if not parseable."""
    stem = port_file.stem  # e.g. "CortexPort-51372"
    if stem.startswith("CortexPort-"):
        raw = stem[len("CortexPort-"):]
        if raw.isdigit():
            return int(raw)
    return 0


def _parse_port_file(
    port_file: pathlib.Path,
) -> EditorConnection | None:
    """Parse a CortexPort-{PID}.txt file into an EditorConnection."""
    pid_from_name = _pid_from_port_filename(port_file)
    try:
        content = port_file.read_text().strip()
        if content.startswith("{"):
            data = json.loads(content)
            port = int(data["port"])
            pid = data.get("pid")
            started_at = data.get("started_at", "")
            if pid is not None:
                pid = int(pid)
            else:
                pid = pid_from_name
            logger.info("Parsed port file %s: port=%d pid=%s", port_file.name, port, pid)
            return EditorConnection(
                port=port,
                pid=pid,
                started_at=started_at,
                port_file=port_file,
                project=data.get("project", ""),
            )

        # Legacy plain integer format
        port = int(content)
        logger.info("Parsed legacy port file %s: port=%d", port_file.name, port)
        return EditorConnection(
            port=port,
            pid=pid_from_name,
            started_at="",
            port_file=port_file,
            project="",
        )
    except (ValueError, OSError, json.JSONDecodeError, KeyError) as e:
        logger.warning("Failed to read port file %s: %s", port_file, e)
        return None


def _find_saved_dir() -> pathlib.Path | None:
    """Find the Saved/ directory for the current project."""
    project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    if project_dir:
        saved = pathlib.Path(project_dir) / "Saved"
        return saved if saved.is_dir() else None

    current = pathlib.Path(__file__).resolve().parent
    for _ in range(20):
        if list(current.glob("*.uproject")):
            saved = current / "Saved"
            return saved if saved.is_dir() else None
        parent = current.parent
        if parent == current:
            break
        current = parent
    return None


def _discover_all_editors() -> list[EditorConnection]:
    """Discover all live Unreal Editor instances from port files.

    Returns list of EditorConnection for all editors whose PIDs are alive.
    """
    saved_dir = _find_saved_dir()
    if saved_dir is None:
        return []

    port_files = list(saved_dir.glob("CortexPort-*.txt"))
    if not port_files:
        return []

    editors = []
    for pf in port_files:
        conn = _parse_port_file(pf)
        if conn is None:
            continue
        if conn.pid and not _is_editor_alive(conn.pid):
            logger.debug("Skipping stale port file %s (PID %d dead)", pf.name, conn.pid)
            continue
        editors.append(conn)

    if len(editors) > 1:
        logger.warning(
            "Multiple live editors detected: %s",
            ", ".join(f"PID {e.pid} on port {e.port}" for e in editors),
        )

    return editors


def _read_project_path(editor: EditorConnection) -> str | None:
    """Read project_path from an EditorConnection's port file."""
    try:
        content = editor.port_file.read_text().strip()
        if content.startswith("{"):
            data = json.loads(content)
            return data.get("project_path")
    except (OSError, json.JSONDecodeError):
        pass
    return None


def _discover_port() -> tuple[int, int | None, str | None, str] | None:
    """Discover the Cortex TCP port from per-PID port files.

    Selection priority:
    1. CORTEX_EDITOR_PID env var - select matching PID
    2. Most recently started editor (by started_at field)

    Returns:
        Tuple of (port, pid, project_path, project_name), or None if not found.
    """
    editors = _discover_all_editors()
    if not editors:
        return None

    target_pid_str = os.environ.get("CORTEX_EDITOR_PID")
    if target_pid_str:
        try:
            target_pid = int(target_pid_str)
        except ValueError:
            logger.warning("Invalid CORTEX_EDITOR_PID: %s", target_pid_str)
            target_pid = None

        if target_pid:
            for editor in editors:
                if editor.pid == target_pid:
                    logger.info("Selected editor PID %d (via CORTEX_EDITOR_PID)", target_pid)
                    return editor.port, editor.pid, _read_project_path(editor), editor.project
            logger.warning("CORTEX_EDITOR_PID=%d not found among live editors", target_pid)
            return None

    editors.sort(key=lambda e: e.started_at, reverse=True)
    selected = editors[0]
    logger.info("Selected most recent editor: PID %d on port %d", selected.pid, selected.port)
    return selected.port, selected.pid, _read_project_path(selected), selected.project


class UEConnection:
    """Manages TCP connection to the UnrealCortex plugin."""

    def __init__(self, host: str = "127.0.0.1", port: int | None = None):
        self.host = host
        self._pid: int | None = None
        self._project_path: str | None = None
        self._expected_project: str = ""
        if port is not None:
            self.port = port
        else:
            # Try port file discovery, then fallback to default
            discovered = _discover_port()
            if discovered is not None:
                self.port, self._pid, self._project_path, self._expected_project = discovered
            else:
                self.port = _DEFAULT_PORT
        self._socket: socket.socket | None = None
        self._cache = ResponseCache()
        self._recv_buffer = b""
        self._socket_lock = threading.Lock()
        self._loaded_file_cache = False

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
            self._validate_project()
            try:
                self._send_and_receive("get_capabilities", {})
            except Exception as e:
                logger.warning("Failed to fetch capabilities during handshake: %s", e)
            if not self._loaded_file_cache:
                self.load_file_caches()
                self._loaded_file_cache = True
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
                    # Re-discover port in case editor restarted on a different port
                    result = _discover_port()
                    if result is not None:
                        new_port, new_pid, new_project_path, new_project = result
                        if new_port != self.port:
                            logger.info(
                                "Port changed %d → %d, updating", self.port, new_port
                            )
                            self.port = new_port
                        self._pid = new_pid
                        self._project_path = new_project_path
                        self._expected_project = new_project
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

    def _find_cache_dir(self) -> pathlib.Path | None:
        """Find Saved/Cortex/ directory."""
        project_dir = os.environ.get("CORTEX_PROJECT_DIR")
        if project_dir:
            found = pathlib.Path(project_dir) / "Saved" / "Cortex"
            return found if found.exists() else None

        current = pathlib.Path(__file__).resolve().parent
        for _ in range(20):
            if list(current.glob("*.uproject")):
                found = current / "Saved" / "Cortex"
                return found if found.exists() else None
            parent = current.parent
            if parent == current:
                break
            current = parent
        return None

    def load_file_caches(self) -> None:
        """Load cached responses from Saved/Cortex/ files if fresh."""
        cache_dir = self._find_cache_dir()
        if cache_dir is None:
            return

        reflect_file = cache_dir / "reflect-cache.json"
        if not reflect_file.exists():
            return

        try:
            cache = json.loads(reflect_file.read_text())
            age = time.time() - reflect_file.stat().st_mtime
            if age > 3600:
                logger.info("Reflect cache too old (%ds), skipping", int(age))
                return

            data = cache.get("data", {})
            if data.get("total_classes") is None or "name" not in data:
                logger.warning("Reflect cache has unexpected shape, skipping")
                return

            params = cache.get(
                "params",
                {
                    "root": "AActor",
                    "depth": 10,
                    "max_results": 5000,
                    "include_engine": False,
                },
            )
            key = self._cache.make_key("reflect.class_hierarchy", params)
            self._cache.set(
                key,
                {"success": True, "data": data},
                ttl=max(1, 3600 - int(age)),
            )
            logger.info(
                "Loaded reflect cache from file (age: %ds, %d classes)",
                int(age),
                data.get("total_classes", 0),
            )
        except (json.JSONDecodeError, OSError) as e:
            logger.warning("Failed to load reflect cache: %s", e)

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

    def _validate_project(self) -> None:
        """Warn if the connected editor project name differs from expected."""
        if self._socket is None or not self._expected_project:
            return

        try:
            request = json.dumps({"command": "get_status", "params": {}}) + "\n"
            self._socket.sendall(request.encode("utf-8"))

            self._socket.settimeout(5.0)
            data = b""
            while b"\n" not in data:
                chunk = self._socket.recv(4096)
                if not chunk:
                    break
                data += chunk

            if data:
                first_line, remainder = data.split(b"\n", 1)
                if remainder:
                    self._recv_buffer = remainder + self._recv_buffer
                response = json.loads(first_line.decode("utf-8"))
                actual_project = response.get("data", {}).get("project_name", "")
                if actual_project and actual_project != self._expected_project:
                    logger.warning(
                        "Connected to '%s' but expected '%s' — wrong editor on port %d. "
                        "Check for stale port files in Saved/.",
                        actual_project,
                        self._expected_project,
                        self.port,
                    )
        except (OSError, json.JSONDecodeError, UnicodeDecodeError) as e:
            logger.debug("Project validation skipped: %s", e)
        finally:
            try:
                self._socket.settimeout(_RECV_TIMEOUT)
            except OSError:
                pass
