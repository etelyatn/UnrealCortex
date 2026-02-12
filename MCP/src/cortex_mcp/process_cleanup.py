"""Cleanup stale cortex-mcp processes before server startup."""

import logging
import os
import sys

logger = logging.getLogger(__name__)


def cleanup_stale_processes():
    """Kill any existing cortex-mcp processes before starting new server.

    This prevents "file in use" errors when uv tries to reinstall the package.
    Only kills processes on Windows (where the issue occurs).
    """
    if sys.platform != "win32":
        return

    try:
        import psutil
        current_pid = os.getpid()
        killed = []

        for proc in psutil.process_iter(['pid', 'name', 'exe']):
            try:
                # Match cortex-mcp.exe or python.exe running cortex_mcp
                name = proc.info['name']
                exe = proc.info['exe']
                pid = proc.info['pid']

                if pid == current_pid:
                    continue

                if name == 'cortex-mcp.exe' or (exe and 'cortex-mcp' in exe):
                    proc.kill()
                    killed.append(f"{name} (PID {pid})")
                    logger.info(f"Killed stale process: {name} (PID {pid})")
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue

        if killed:
            logger.info(f"Cleaned up {len(killed)} stale process(es): {', '.join(killed)}")

    except ImportError:
        # psutil not available - log warning but continue
        logger.warning("psutil not installed - cannot cleanup stale processes. "
                      "Install with: uv add psutil")
    except Exception as e:
        # Don't fail startup if cleanup fails
        logger.warning(f"Failed to cleanup stale processes: {e}")
