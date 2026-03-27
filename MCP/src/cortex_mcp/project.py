"""Shared project directory resolution for the Cortex MCP server.

Provides a single source of truth for resolving the Unreal project root
from environment variables and filesystem heuristics.
"""

import functools
import logging
import os
import pathlib

logger = logging.getLogger(__name__)


@functools.lru_cache(maxsize=1)
def _walk_up_for_uproject() -> pathlib.Path | None:
    """Walk up from this package's location to find *.uproject."""
    current = pathlib.Path(__file__).resolve().parent
    for _ in range(20):
        if list(current.glob("*.uproject")):
            return current
        parent = current.parent
        if parent == current:
            break
        current = parent
    return None


def resolve_project_dir() -> pathlib.Path | None:
    """Resolve the Unreal project root directory.

    Resolution order:
    1. CORTEX_PROJECT_DIR env var (absolute used as-is; relative resolved
       against the .uproject walk-up root, NOT CWD)
    2. CLAUDE_PROJECT_DIR env var (set by Claude Code to the workspace root)
    3. Walk up from this package's location to find *.uproject

    Returns:
        Absolute Path to the project root, or None if not found.
    """
    # 1. CORTEX_PROJECT_DIR
    cortex_dir = os.environ.get("CORTEX_PROJECT_DIR")
    if cortex_dir:
        project_dir = pathlib.Path(cortex_dir)
        if project_dir.is_absolute():
            return project_dir
        # Relative — resolve against walk-up root, not CWD
        walk_up_root = _walk_up_for_uproject()
        if walk_up_root is not None:
            return (walk_up_root / project_dir).resolve()
        # Relative but no walk-up root — try CLAUDE_PROJECT_DIR as base
        claude_dir = os.environ.get("CLAUDE_PROJECT_DIR")
        if claude_dir:
            return (pathlib.Path(claude_dir) / cortex_dir).resolve()
        logger.warning(
            "CORTEX_PROJECT_DIR='%s' is relative but no project root found to resolve against",
            cortex_dir,
        )
        return None

    # 2. CLAUDE_PROJECT_DIR
    claude_dir = os.environ.get("CLAUDE_PROJECT_DIR")
    if claude_dir:
        claude_path = pathlib.Path(claude_dir)
        if claude_path.is_dir() and list(claude_path.glob("*.uproject")):
            return claude_path.resolve()
        elif claude_path.is_dir():
            logger.debug(
                "CLAUDE_PROJECT_DIR='%s' has no *.uproject, skipping",
                claude_dir,
            )

    # 3. Walk up from package location
    return _walk_up_for_uproject()


def resolve_saved_dir() -> pathlib.Path | None:
    """Resolve the Saved/ directory for the current project.

    Returns:
        Absolute Path to the Saved/ directory, or None if not found.
    """
    project_root = resolve_project_dir()
    if project_root is None:
        return None
    saved = project_root / "Saved"
    return saved if saved.is_dir() else None
