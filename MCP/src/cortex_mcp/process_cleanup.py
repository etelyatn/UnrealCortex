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
        current_proc = psutil.Process(os.getpid())
        current_pid = current_proc.pid
        parent_pid = current_proc.ppid()

        # Build set of PIDs in our own process tree (self + ancestors)
        own_pids = {current_pid, parent_pid}
        try:
            parent = current_proc.parent()
            if parent:
                own_pids.add(parent.ppid())  # grandparent (uv.exe)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

        killed = []
        for proc in psutil.process_iter(['pid', 'name', 'exe']):
            try:
                name = proc.info['name']
                exe = proc.info['exe']
                pid = proc.info['pid']

                if pid in own_pids:
                    continue

                if name == 'cortex-mcp.exe' or (exe and 'cortex-mcp' in exe):
                    proc.kill()
                    killed.append(f"{name} (PID {pid})")
                    logger.info("Killed stale process: %s (PID %d)", name, pid)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue

        if killed:
            logger.info("Cleaned up %d stale process(es)", len(killed))

    except ImportError:
        logger.warning("psutil not installed - cannot cleanup stale processes")
    except Exception as e:
        logger.warning("Failed to cleanup stale processes: %s", e)
