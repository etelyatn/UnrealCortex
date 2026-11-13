"""Gen composite tool — submit a generation job and wait for completion."""

from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Optional

from cortex_mcp.tcp_client import UECommandError

logger = logging.getLogger(__name__)

_TERMINAL_STATUSES = {"imported", "failed", "cancelled", "download_failed", "import_failed"}


async def _send(connection, command: str, params: dict) -> dict:
    """Run one blocking editor command off the event loop; failures become result dicts."""
    try:
        return await asyncio.to_thread(connection.send_command, command, params)
    except UECommandError as exc:
        return {
            "success": False,
            "error": f"{exc.code}: {exc.message}",
            "details": exc.details,
        }
    except ConnectionError as exc:
        return {"success": False, "error": str(exc)}


async def _submit_and_wait(
    connection,
    command: str,
    params: dict,
    poll_interval: float = 5.0,
    timeout: float = 300.0,
) -> str:
    """Submit a gen job and poll until terminal state or timeout."""
    # Submit
    submit_result = await _send(connection, command, params)
    if not submit_result.get("success"):
        return json.dumps(submit_result)

    job_id = submit_result["data"]["job_id"]
    start = time.monotonic()

    # Poll with backoff
    interval = poll_interval
    while (time.monotonic() - start) < timeout:
        await asyncio.sleep(interval)

        poll_result = await _send(connection, "gen.job_status", {"job_id": job_id})
        if not poll_result.get("success"):
            return json.dumps(poll_result)

        status = poll_result["data"].get("status", "")
        if status in _TERMINAL_STATUSES:
            return json.dumps(poll_result)

        # Gradual backoff: 5s -> 7.5s -> 10s (cap)
        interval = min(interval * 1.5, 10.0)

    # Timeout
    return json.dumps({
        "success": False,
        "error": f"Timeout after {timeout}s waiting for job {job_id}",
        "data": {"job_id": job_id, "status": "timeout"},
    })


def register_gen_compose_tools(mcp, connection) -> None:
    """Register the gen_compose composite tool."""

    @mcp.tool(
        name="gen_compose",
        description=(
            "COMPOSITE tool — submit an AI generation job and wait for completion. Handles the "
            "submit-poll loop automatically and returns the final job state (imported, failed, "
            "etc.) without manual polling. `type` is one of 'mesh', 'image', 'texturing'; "
            "`prompt` is required for mesh/image and `source_model_path` for texturing. The "
            "remaining fields follow the typed schema (provider/destination/timeout).\n"
        ),
    )
    async def gen_compose(
        type: str,
        prompt: Optional[str] = None,
        source_image_path: Optional[str] = None,
        source_model_path: Optional[str] = None,
        provider: Optional[str] = None,
        destination: Optional[str] = None,
        timeout: float = 300.0,
    ) -> str:
        # Map type to command
        command_map = {
            "mesh": "gen.start_mesh",
            "image": "gen.start_image",
            "texturing": "gen.start_texturing",
        }
        command = command_map.get(type)
        if not command:
            return json.dumps({
                "success": False,
                "error": f"Unknown gen type: '{type}'. Use 'mesh', 'image', or 'texturing'.",
            })

        # Build params
        params: dict = {}
        if prompt:
            params["prompt"] = prompt
        if source_image_path:
            params["source_image_path"] = source_image_path
        if source_model_path:
            params["source_model_path"] = source_model_path
        if provider:
            params["provider"] = provider
        if destination:
            params["destination"] = destination

        return await _submit_and_wait(connection, command, params, timeout=timeout)
