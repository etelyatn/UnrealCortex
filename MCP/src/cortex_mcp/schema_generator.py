"""Generate LLM-readable schema files in .cortex/schema/."""

import logging
import os
import pathlib
import tempfile

logger = logging.getLogger(__name__)

SCHEMA_VERSION = 1


def _get_caller_path() -> pathlib.Path:
    """Get the path of this file (used for .uproject walk-up)."""
    return pathlib.Path(__file__).resolve().parent


def find_project_root() -> pathlib.Path:
    """Find the Unreal project root directory.

    Uses CORTEX_PROJECT_DIR env var if set, otherwise walks up to find .uproject.
    """
    project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    if project_dir:
        return pathlib.Path(project_dir)

    current = _get_caller_path()
    for _ in range(20):
        if list(current.glob("*.uproject")):
            return current
        parent = current.parent
        if parent == current:
            break
        current = parent

    raise FileNotFoundError("Cannot find .uproject file. Set CORTEX_PROJECT_DIR env var.")


def get_schema_dir() -> pathlib.Path:
    """Get the .cortex/schema/ directory path."""
    return find_project_root() / ".cortex" / "schema"


def atomic_write(path: pathlib.Path, content: str) -> None:
    """Write content to a file atomically via temp file + rename."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_fd, tmp_path = tempfile.mkstemp(
        suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(tmp_fd, "w", encoding="utf-8") as f:
            f.write(content)
        os.replace(tmp_path, path)
    except Exception:
        # Clean up temp file on failure
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise
