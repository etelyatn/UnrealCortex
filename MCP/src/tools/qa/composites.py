"""QA composite workflow tools."""

from cortex_mcp.qa import detect_structural_issues, write_report_bundle
from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

# One page of the finite editor log window; the native read caps a page at 200 retained rows, so a
# page never has to be shortened after the fact.
LOG_WINDOW_PAGE_LIMIT = 200
# Runaway guard for a non-advancing window; this is a bound, never a retry or poll loop.
LOG_WINDOW_MAX_PAGES = 50


def _log_window_baseline(connection):
    """Pre-action watermark: the newest retained cursor before the first step's action runs.

    Returns ``(start_cursor, failure)``. ``failure`` is a non-None string only when the watermark
    could not be read, so a failed count-only read is never mistaken for a genuinely empty pre-action
    buffer (which would silently attribute every retained record to the current action). The
    watermark uses the one-shot transport: it is never retried or polled.
    """
    try:
        response = connection.send_command_once(
            "editor.get_recent_logs", {"count_only": True, "since_seconds": -1}
        )
    except (ConnectionError, RuntimeError, OSError) as error:
        return None, f"log window baseline failed: {error}"

    data = response.get("data", {}) if isinstance(response, dict) else {}
    latest = data.get("latest_cursor")
    if not isinstance(latest, (int, float)):
        return None, "log window baseline returned no latest_cursor"
    return int(latest), None


def _read_log_window(connection, start_cursor, baseline_failure=None):
    """Collect the finite editor log window that starts after ``start_cursor`` for one action.

    Returns ``(entries, coverage)``. The first page fixes the end-of-step watermark, so every later
    page is pinned with ``until_cursor``; a record added after the window end still belongs to the
    next action's window and is never attributed late. Coverage is complete only when the whole fixed
    window was read: front eviction, a native/MCP size refusal, a connection failure, non-advancing
    pagination, a failed or unavailable watermark, or the page-cap guard leaves ``complete=False``
    with the entries read so far and an explicit cause. Every read uses the one-shot transport (no
    retry, no poll), and an applied action's success is never overwritten.
    """
    entries: list[dict] = []
    coverage: dict = {
        "complete": True,
        "lost_before_cursor": False,
        "window_end_cursor": None,
        "count": 0,
        "cause": None,
    }

    # Without a watermark the window is unbounded; reading from the beginning would misattribute
    # retained records to the current action, so no read is attempted.
    if baseline_failure is not None:
        coverage["complete"] = False
        coverage["cause"] = baseline_failure
        return entries, coverage

    if start_cursor is None:
        coverage["complete"] = False
        coverage["cause"] = "log window start cursor is unavailable"
        return entries, coverage

    cursor = int(start_cursor)
    window_end = None

    for _ in range(LOG_WINDOW_MAX_PAGES):
        params = {"since_cursor": cursor, "since_seconds": -1, "limit": LOG_WINDOW_PAGE_LIMIT}
        if window_end is not None:
            params["until_cursor"] = window_end

        try:
            response = connection.send_command_once("editor.get_recent_logs", params)
        except (ConnectionError, RuntimeError, OSError) as error:
            coverage["complete"] = False
            coverage["cause"] = f"log window read failed: {error}"
            break

        page = response.get("data", {}) if isinstance(response, dict) else {}
        if "_error" in page:
            coverage["complete"] = False
            coverage["cause"] = page.get("_message") or page.get("_error")
            break

        entries.extend(entry for entry in page.get("entries", []) if isinstance(entry, dict))
        coverage["count"] = len(entries)

        if window_end is None:
            window_end = page.get("window_end_cursor")
            coverage["window_end_cursor"] = window_end

        if page.get("lost_before_cursor"):
            coverage["lost_before_cursor"] = True
            coverage["complete"] = False
            coverage["cause"] = "front log records were evicted before the window start"
            break

        if not page.get("has_more"):
            break

        next_cursor = page.get("next_cursor")
        if not isinstance(next_cursor, (int, float)) or int(next_cursor) <= cursor:
            coverage["complete"] = False
            coverage["cause"] = "log window pagination did not advance"
            break

        cursor = int(next_cursor)
    else:
        coverage["complete"] = False
        coverage["cause"] = "log window page cap reached"

    return entries, coverage


def register_qa_composite_tools(mcp, connection: UEConnection):
    """Register QA composite tools."""

    @mcp.tool()
    def test_step(
        action: dict,
        wait: dict | None = None,
        assertion: dict | None = None,
        screenshot_name: str = "qa_step.png",
    ) -> str:
        try:
            start_cursor, baseline_failure = _log_window_baseline(connection)

            action_params = action.get("params", {})
            action_duration = float(action_params.get("duration", 0))
            action_timeout = float(action_params.get("timeout", 5.0))
            action_response = connection.send_command(
                action["command"], action_params, timeout=max(action_duration, action_timeout) + 5.0
            )

            wait_response = None
            if wait is not None:
                wait_timeout = float(wait.get("params", {}).get("timeout", 5.0)) + 5.0
                wait_response = connection.send_command(
                    wait["command"],
                    wait.get("params", {}),
                    timeout=wait_timeout,
                )

            observed = connection.send_command("qa.observe_state", {})
            log_entries, log_coverage = _read_log_window(connection, start_cursor, baseline_failure)

            assert_response = None
            if assertion is not None:
                assert_response = connection.send_command(assertion["command"], assertion.get("params", {}))

            screenshot = connection.send_command(
                "editor.capture_screenshot",
                {"filename": screenshot_name},
                timeout=60.0,
            )

            findings = detect_structural_issues(
                observed.get("data", {}),
                wait_response.get("data", {}) if wait_response else None,
                {"entries": log_entries},
            )

            summary = {
                "action_success": action_response.get("success", False),
                "wait_success": wait_response.get("success", True) if wait_response else True,
                "assert_success": assert_response.get("success", True) if assert_response else True,
                "finding_count": len(findings),
                "log_coverage": log_coverage,
                "screenshot": screenshot.get("data", {}),
            }
            return format_response(summary, "test_step")
        except (RuntimeError, ConnectionError, KeyError) as e:
            return f"Error: {e}"

    @mcp.tool()
    def run_scenario_inline(
        scenario_name: str,
        steps: list[dict],
        verbose: bool = False,
    ) -> str:
        try:
            results: list[dict] = []
            findings: list[dict] = []

            # One pre-action watermark bounds the first step's window; every later step continues
            # from the previous window's fixed end, so a record that arrives between windows is
            # attributed to exactly the next action instead of falling between two baselines.
            start_cursor, baseline_failure = _log_window_baseline(connection)

            for index, step in enumerate(steps):
                command = step.get("command", "")
                params = step.get("params", {})
                duration = float(params.get("duration", 0))
                base_timeout = float(params.get("timeout", 5.0))
                timeout = max(duration, base_timeout) + 5.0

                response = connection.send_command(command, params, timeout=timeout)
                observed = connection.send_command("qa.observe_state", {})
                log_entries, log_coverage = _read_log_window(
                    connection,
                    start_cursor,
                    baseline_failure if index == 0 else None,
                )
                if log_coverage["window_end_cursor"] is not None:
                    start_cursor = log_coverage["window_end_cursor"]

                step_findings = detect_structural_issues(
                    observed.get("data", {}),
                    response.get("data", {}),
                    {"entries": log_entries},
                )
                findings.extend(step_findings)

                step_result = {
                    "index": index,
                    "command": command,
                    "success": response.get("success", False),
                    "log_coverage": log_coverage,
                    "findings": step_findings,
                }
                if verbose:
                    step_result["response"] = response.get("data", {})
                results.append(step_result)

            summary_lines = [
                f"# QA Scenario Report: {scenario_name}",
                "",
                f"- Steps: {len(steps)}",
                f"- Findings: {len(findings)}",
                f"- Passed steps: {sum(1 for s in results if s['success'])}",
            ]
            report = write_report_bundle(scenario_name, summary_lines, findings)

            payload = {"scenario": scenario_name, "steps": results, "report": report}
            return format_response(payload, "run_scenario_inline")
        except (RuntimeError, ConnectionError, ValueError, KeyError) as e:
            return f"Error: {e}"
