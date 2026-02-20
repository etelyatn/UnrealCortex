"""QA composite workflow tools."""

from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection
from .detector import detect_structural_issues
from .reporter import write_report_bundle


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
            logs = connection.send_command("editor.get_recent_logs", {"max_lines": 100})

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
                logs.get("data", {}),
            )

            summary = {
                "action_success": action_response.get("success", False),
                "wait_success": wait_response.get("success", True) if wait_response else True,
                "assert_success": assert_response.get("success", True) if assert_response else True,
                "finding_count": len(findings),
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
            log_cursor = 0

            for index, step in enumerate(steps):
                command = step.get("command", "")
                params = step.get("params", {})
                duration = float(params.get("duration", 0))
                base_timeout = float(params.get("timeout", 5.0))
                timeout = max(duration, base_timeout) + 5.0

                response = connection.send_command(command, params, timeout=timeout)
                observed = connection.send_command("qa.observe_state", {})
                logs = connection.send_command(
                    "editor.get_recent_logs",
                    {"cursor": log_cursor, "max_lines": 200},
                )
                log_data = logs.get("data", {})
                log_cursor = int(log_data.get("next_cursor", log_cursor))

                step_findings = detect_structural_issues(
                    observed.get("data", {}),
                    response.get("data", {}),
                    log_data,
                )
                findings.extend(step_findings)

                step_result = {
                    "index": index,
                    "command": command,
                    "success": response.get("success", False),
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
