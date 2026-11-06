"""Unit tests for QA MCP tools."""

import json
import sys
from pathlib import Path
from unittest.mock import MagicMock, patch

# Add tools/ to path for imports (matching existing tests)
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from qa.actions import register_qa_action_tools
from qa.assertions import register_qa_assertion_tools
from qa.composites import register_qa_composite_tools
from qa.detector import detect_structural_issues as tools_detect_structural_issues
from qa.setup import register_qa_setup_tools
from qa.world import register_qa_world_tools
import qa.composites as qa_composites
from cortex_mcp.qa.detector import detect_structural_issues
from cortex_mcp.qa.reporter import write_report_bundle


class MockMCP:
    """Captures tools registered via @mcp.tool()."""

    def __init__(self):
        self.tools = {}

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator


class FakeLogWindowConnection:
    """Finite editor log-window double for the QA composites.

    Models the native ``editor.get_recent_logs`` contract instead of echoing a stale field shape:
    a finite retained window addressed by cursors, count-only watermarks, ``since_cursor`` /
    ``until_cursor`` pagination with a ``limit`` cap, ``since_seconds`` time selection (native
    default 30s, ``-1`` disables, any other negative refuses), front-eviction loss, and records that
    arrive after a fixed window end. It also answers the minimal non-log commands a QA step issues
    (``qa.observe_state``, the step action, the screenshot). The stale ``max_lines`` / Python
    ``cursor`` consumer forms are rejected, and the one-shot transport (``send_command_once``) is
    recorded separately from the retrying ``send_command``.
    """

    NATIVE_DEFAULT_SINCE_SECONDS = 30.0

    def __init__(
        self,
        records,
        *,
        window_end=None,
        evicted_before=0,
        watermark=None,
        pending_after_first_window=None,
        count_only_error=None,
    ):
        self.records = sorted(records, key=lambda record: record["cursor"])
        # window_end fixes the end-of-window cursor; None tracks the newest retained record.
        self.window_end = window_end
        # evicted_before is the highest cursor dropped from the front of the ring buffer.
        self.evicted_before = evicted_before
        # watermark is the pre-action latest cursor reported by a count-only read; it is the
        # baseline the first step's window starts from, so records added by the action are paged back.
        self.watermark = self.latest_cursor if watermark is None else watermark
        # pending_after_first_window models a record that arrives once the previous action's window
        # has been fixed: it must be attributed to the next window, never dropped between baselines.
        self.pending_after_first_window = list(pending_after_first_window or [])
        # count_only_error makes the count-only watermark fail, like a dropped connection.
        self.count_only_error = count_only_error
        self._windows_read = 0
        self.calls = []
        self.once_calls = []
        self.retrying_calls = []

    @property
    def latest_cursor(self):
        return self.records[-1]["cursor"] if self.records else -1

    @property
    def oldest_available_cursor(self):
        return self.records[0]["cursor"] if self.records else None

    @property
    def resolved_window_end(self):
        return self.window_end if self.window_end is not None else self.latest_cursor

    # -- transport --

    def send_command(self, command, params=None, timeout=None):
        """The retrying transport; a QA log read must never use it."""
        return self._handle(command, params, once=False)

    def send_command_once(self, command, params=None, timeout=None):
        """The one-shot transport the QA log window collector must use."""
        return self._handle(command, params, once=True)

    def _handle(self, command, params=None, once=False):
        if command == "qa.observe_state":
            return {
                "success": True,
                "data": {"player": {"location": [0.0, 0.0, 0.0]}, "nearby_actors": []},
            }
        if command == "editor.capture_screenshot":
            return {
                "success": True,
                "data": {"path": "Saved/Screenshots/qa_step.png"},
            }
        if command != "editor.get_recent_logs":
            return {"success": True, "data": {"success": True}}

        params = dict(params or {})
        self.calls.append(params)
        (self.once_calls if once else self.retrying_calls).append(params)
        assert "max_lines" not in params, f"stale max_lines log read: {params}"
        assert "cursor" not in params, f"python cursor is unsupported for native log windows: {params}"
        assert params.get("count_only") or "since_cursor" in params, (
            f"log window read must use a count-only watermark or a since_cursor: {params}"
        )

        if params.get("count_only"):
            if self.count_only_error is not None:
                raise ConnectionError(self.count_only_error)
            return {
                "success": True,
                "data": {
                    "entries": [],
                    "count": 0,
                    "matched_count": 0,
                    "has_more": False,
                    "next_cursor": self.watermark,
                    "latest_cursor": self.watermark,
                    "window_end_cursor": self.resolved_window_end,
                    "oldest_available_cursor": self.oldest_available_cursor,
                    "lost_before_cursor": False,
                },
            }

        since_seconds = float(params.get("since_seconds", self.NATIVE_DEFAULT_SINCE_SECONDS))
        if since_seconds < 0 and since_seconds != -1.0:
            return {"success": False, "data": {}, "error": "INVALID_SINCE_SECONDS"}

        since_cursor = int(params.get("since_cursor", -1))
        limit = int(params.get("limit", 200))
        requested_end = params.get("until_cursor")
        window_end = self.resolved_window_end if requested_end is None else int(requested_end)

        retained = [record for record in self.records if record["cursor"] <= window_end]
        selectable = [record for record in retained if record["cursor"] > since_cursor]

        if since_seconds != -1.0:
            latest_timestamp = max((record["timestamp"] for record in retained), default=None)
            if latest_timestamp is not None:
                cutoff = latest_timestamp - since_seconds
                selectable = [record for record in selectable if record["timestamp"] >= cutoff]

        page = selectable[:limit]
        next_cursor = page[-1]["cursor"] if page else since_cursor
        requested_start = since_cursor + 1 if since_cursor >= 0 else 1
        latest_before = self.latest_cursor
        oldest_before = self.oldest_available_cursor

        response = {
            "success": True,
            "data": {
                "entries": [
                    {
                        "cursor": record["cursor"],
                        "timestamp": record["timestamp"],
                        "severity": record["severity"],
                        "message": record["message"],
                    }
                    for record in page
                ],
                "count": len(page),
                "matched_count": len(selectable),
                "has_more": len(selectable) > len(page),
                "next_cursor": next_cursor,
                "latest_cursor": latest_before,
                "window_end_cursor": window_end,
                "oldest_available_cursor": oldest_before,
                "lost_before_cursor": requested_start <= self.evicted_before,
            },
        }

        # A pending record becomes retained once this window has been fixed, exactly like a row that
        # arrives after the previous action's window end; a later count-only read sees it too.
        if self._windows_read == 0 and self.pending_after_first_window:
            self.records.extend(self.pending_after_first_window)
            self.records.sort(key=lambda record: record["cursor"])
            self.watermark = max(self.watermark, self.latest_cursor)
            self.pending_after_first_window = []
        self._windows_read += 1

        return response


def _log_window_helper():
    """Return the finite log-window collector the QA composites expose."""
    helper = getattr(qa_composites, "_read_log_window", None)
    assert callable(helper), "qa.composites._read_log_window is missing"
    return helper


def _coverage_value(coverage, key):
    if isinstance(coverage, dict):
        return coverage.get(key)
    return getattr(coverage, key, None)


def _register_composites(connection):
    mcp = MockMCP()
    register_qa_composite_tools(mcp, connection)
    return mcp


def test_world_tools_registration_and_wiring():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"nearby_actor_count": 1}}

    register_qa_world_tools(mcp, connection)

    assert "observe_game_state" in mcp.tools
    assert "get_actor_details" in mcp.tools
    assert "get_player_details" in mcp.tools

    result = mcp.tools["observe_game_state"](include_los=True)
    parsed = json.loads(result)
    assert parsed["nearby_actor_count"] == 1
    connection.send_command.assert_called_with(
        "qa.observe_state", {
            "radius": 5000.0,
            "max_actors": 20,
            "include_los": True,
            "interaction_range": 200.0,
        }
    )


def test_action_tools_send_deferred_timeouts():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"arrived": True}}

    register_qa_action_tools(mcp, connection)
    result = mcp.tools["move_player_to"](
        target=[100.0, 0.0, 0.0], timeout=7.0, acceptance_radius=50.0
    )

    parsed = json.loads(result)
    assert parsed["arrived"] is True
    connection.send_command.assert_called_with(
        "qa.move_to",
        {"target": [100.0, 0.0, 0.0], "timeout": 7.0, "acceptance_radius": 50.0},
        timeout=12.0,
    )


def test_interact_default_duration_sends_tap():
    """interact_with with no duration sends default 0.1s tap."""
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"success": True, "key": "E", "duration": 0.1}}

    register_qa_action_tools(mcp, connection)
    result = mcp.tools["interact_with"](key="E")

    parsed = json.loads(result)
    assert parsed["success"] is True
    connection.send_command.assert_called_with(
        "qa.interact",
        {"key": "E", "duration": 0.1},
        timeout=5.1,
    )


def test_interact_hold_duration_sends_timeout():
    """interact_with with duration=2.0 sends hold and sets TCP timeout."""
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"success": True, "key": "LeftMouseButton", "duration": 2.0}}

    register_qa_action_tools(mcp, connection)
    result = mcp.tools["interact_with"](key="LeftMouseButton", duration=2.0)

    parsed = json.loads(result)
    assert parsed["success"] is True
    connection.send_command.assert_called_with(
        "qa.interact",
        {"key": "LeftMouseButton", "duration": 2.0},
        timeout=7.0,
    )


def test_interact_with_target_and_duration():
    """interact_with sends both target and duration when specified."""
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"success": True, "key": "LeftMouseButton", "duration": 2.0}}

    register_qa_action_tools(mcp, connection)
    mcp.tools["interact_with"](target="SolderPoint_01", key="LeftMouseButton", duration=2.0)

    connection.send_command.assert_called_with(
        "qa.interact",
        {"key": "LeftMouseButton", "duration": 2.0, "target": "SolderPoint_01"},
        timeout=7.0,
    )


def test_wait_for_condition_timeout_forwarding():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"condition_met": True}}

    register_qa_action_tools(mcp, connection)
    result = mcp.tools["wait_for_condition"](
        type="actor_visible",
        timeout=9.0,
        actor="Door_01",
    )

    parsed = json.loads(result)
    assert parsed["condition_met"] is True
    connection.send_command.assert_called_with(
        "qa.wait_for",
        {"type": "actor_visible", "timeout": 9.0, "actor": "Door_01"},
        timeout=14.0,
    )


def test_setup_tools_wiring():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.return_value = {"data": {"ok": True}}

    register_qa_setup_tools(mcp, connection)
    mcp.tools["set_random_seed"](123)
    connection.send_command.assert_called_with("qa.set_random_seed", {"seed": 123})


def test_assertion_tool_captures_screenshot_on_failure():
    mcp = MockMCP()
    connection = MagicMock()
    connection.send_command.side_effect = [
        {"data": {"passed": False, "message": "failed"}},
        {"data": {"path": "Saved/Screenshots/qa_assertion_failed.png"}},
    ]

    register_qa_assertion_tools(mcp, connection)
    result = mcp.tools["assert_game_state"](type="actor_visible", actor="Door_01")
    parsed = json.loads(result)
    assert parsed["passed"] is False

    assert connection.send_command.call_count == 2
    assert connection.send_command.call_args_list[1].args[0] == "editor.capture_screenshot"


def test_composite_test_step_aggregates_summary():
    connection = FakeLogWindowConnection([])
    mcp = _register_composites(connection)

    result = mcp.tools["test_step"](
        action={"command": "qa.interact", "params": {"key": "E"}},
        wait={"command": "qa.wait_for", "params": {"type": "delay", "timeout": 1.0}},
        assertion={"command": "qa.assert_state", "params": {"type": "delay"}},
    )
    parsed = json.loads(result)
    assert parsed["action_success"] is True
    assert parsed["assert_success"] is True
    assert parsed["finding_count"] == 0
    assert parsed["log_coverage"]["complete"] is True
    assert parsed["screenshot"]["path"] == "Saved/Screenshots/qa_step.png"


def test_run_scenario_inline_uses_finite_log_window():
    """run_scenario_inline collects finite cursor-only windows that continue across steps."""
    connection = FakeLogWindowConnection(
        [
            {"cursor": 1, "timestamp": 101.0, "severity": "log", "message": "step one"},
            {"cursor": 2, "timestamp": 101.1, "severity": "log", "message": "step two"},
        ],
        watermark=0,
    )
    mcp = _register_composites(connection)

    scenario = [
        {"command": "qa.look_at", "params": {"target": "Door_01"}},
        {"command": "qa.wait_for", "params": {"type": "delay", "timeout": 1.0}},
    ]
    with patch(
        "qa.composites.write_report_bundle",
        return_value={
            "report_dir": "QA/reports/mock",
            "summary_path": "QA/reports/mock/summary.md",
            "findings_path": "QA/reports/mock/findings.json",
            "finding_count": 0,
        },
    ):
        result = mcp.tools["run_scenario_inline"](
            scenario_name="qa-inline",
            steps=scenario,
            verbose=False,
        )
    parsed = json.loads(result)
    assert parsed["scenario"] == "qa-inline"
    assert len(parsed["steps"]) == 2
    assert parsed["report"]["finding_count"] == 0

    assert sum(1 for call in connection.calls if call.get("count_only")) == 1, (
        "one pre-action watermark bounds the first step's window"
    )
    window_calls = [call for call in connection.calls if not call.get("count_only")]
    assert all(call["since_seconds"] == -1 for call in window_calls), (
        "cursor-only windows must disable the 30s time default"
    )
    starts = [int(call["since_cursor"]) for call in window_calls]
    assert starts == sorted(starts), "each step continues from the previous window end"
    assert starts[1] == parsed["steps"][0]["log_coverage"]["window_end_cursor"], (
        "the second step starts exactly at the first step's fixed window end"
    )
    assert connection.retrying_calls == []


def test_run_scenario_inline_interact_duration_timeout():
    """run_scenario_inline calculates timeout from duration for interact commands."""
    connection = FakeLogWindowConnection([])
    mcp = _register_composites(connection)
    delegate = connection.send_command

    def send_command(command, params=None, timeout=None):
        if command == "qa.interact":
            assert timeout >= 15.0, f"Expected timeout >= 15.0 for duration=10.0, got {timeout}"
            return {"success": True, "data": {"success": True, "key": "LeftMouseButton", "duration": 10.0}}
        return delegate(command, params, timeout)

    connection.send_command = send_command

    scenario = [
        {"command": "qa.interact", "params": {"key": "LeftMouseButton", "duration": 10.0}},
    ]
    with patch(
        "qa.composites.write_report_bundle",
        return_value={
            "report_dir": "QA/reports/mock",
            "summary_path": "QA/reports/mock/summary.md",
            "findings_path": "QA/reports/mock/findings.json",
            "finding_count": 0,
        },
    ):
        result = mcp.tools["run_scenario_inline"](
            scenario_name="hold-test",
            steps=scenario,
            verbose=False,
        )
    parsed = json.loads(result)
    assert parsed["steps"][0]["success"] is True


def test_detector_finds_multiple_issue_types():
    findings = detect_structural_issues(
        observed_state={
            "player": {"location": [0.0, 0.0, -20000.0]},
            "nearby_actors": [{"name": "BadActor", "location": [0.2, -0.1, 0.5]}],
            "game_state": {"fps": 10.0},
        },
        wait_result={"timed_out": True},
        recent_logs={"entries": [{"severity": "error", "message": "canonical error without prefix"}]},
    )

    categories = {f["category"] for f in findings}
    assert "physics" in categories
    assert "placement" in categories
    assert "timeout" in categories
    assert "performance" in categories
    assert "logs" in categories


def test_detector_copies_consume_entries_severity_not_logs_level():
    """Both detector copies read the canonical entries/severity shape and drop the stale one."""
    canonical = {"entries": [{"severity": "error", "message": "no textual prefix here"}]}
    stale = {"logs": [{"level": "Error", "message": "Error: legacy shape"}]}

    for label, detect in (
        ("cortex_mcp", detect_structural_issues),
        ("tools", tools_detect_structural_issues),
    ):
        canonical_findings = [
            finding
            for finding in detect(observed_state={}, recent_logs=canonical)
            if finding["category"] == "logs"
        ]
        assert len(canonical_findings) == 1, f"{label} detector missed the canonical severity error"

        stale_findings = [
            finding
            for finding in detect(observed_state={}, recent_logs=stale)
            if finding["category"] == "logs"
        ]
        assert stale_findings == [], f"{label} detector still consumes the stale logs/level shape"


def test_run_scenario_inline_reports_only_error_on_final_page_once():
    """A step whose only error sits past the 200-record page cap is inspected exactly once."""
    records = [
        {
            "cursor": index,
            "timestamp": 100.0 + index * 0.01,
            "severity": "log",
            "message": f"record {index}",
        }
        for index in range(1, 201)
    ]
    records.append(
        {
            "cursor": 201,
            "timestamp": 102.1,
            "severity": "error",
            "message": "final page error without prefix",
        }
    )
    connection = FakeLogWindowConnection(records, watermark=0)
    mcp = _register_composites(connection)

    with patch("qa.composites.write_report_bundle", return_value={"finding_count": 0}):
        result = mcp.tools["run_scenario_inline"](
            scenario_name="final-page",
            steps=[{"command": "qa.look_at", "params": {}}],
            verbose=False,
        )
    parsed = json.loads(result)
    log_findings = [
        finding for finding in parsed["steps"][0]["findings"] if finding["category"] == "logs"
    ]
    assert len(log_findings) == 1, (
        "the only error on the final page must be inspected exactly once (or coverage must "
        "report incomplete)"
    )


def test_run_scenario_inline_cursor_only_detects_error_beyond_time_default():
    """A retained early error is still reported when the step spans longer than the 30s default."""
    records = [{"cursor": 1, "timestamp": 10.0, "severity": "error", "message": "early error without prefix"}]
    records += [
        {"cursor": index, "timestamp": 60.0 + index * 0.01, "severity": "log", "message": f"later {index}"}
        for index in range(2, 8)
    ]
    records.append({"cursor": 8, "timestamp": 70.0, "severity": "log", "message": "end of step"})
    connection = FakeLogWindowConnection(records, watermark=0)
    mcp = _register_composites(connection)

    with patch("qa.composites.write_report_bundle", return_value={"finding_count": 0}):
        result = mcp.tools["run_scenario_inline"](
            scenario_name="long-step",
            steps=[{"command": "qa.wait_for", "params": {"type": "delay", "timeout": 60.0}}],
            verbose=False,
        )
    parsed = json.loads(result)
    log_findings = [
        finding for finding in parsed["steps"][0]["findings"] if finding["category"] == "logs"
    ]
    assert len(log_findings) == 1, (
        "the retained early error must be detected despite a duration beyond the 30s default"
    )


def test_run_scenario_inline_attributes_late_record_to_next_step():
    """A record arriving after a fixed window end is attributed to the next step exactly once."""
    pre_existing = [
        {"cursor": index, "timestamp": 100.0, "severity": "log", "message": f"record {index}"}
        for index in range(1, 201)
    ]
    late_error = {
        "cursor": 201,
        "timestamp": 101.0,
        "severity": "error",
        "message": "late failure without prefix",
    }
    connection = FakeLogWindowConnection(
        pre_existing,
        watermark=200,
        pending_after_first_window=[late_error],
    )
    mcp = _register_composites(connection)

    with patch("qa.composites.write_report_bundle", return_value={"finding_count": 0}):
        result = mcp.tools["run_scenario_inline"](
            scenario_name="carry-forward",
            steps=[
                {"command": "qa.look_at", "params": {}},
                {"command": "qa.wait_for", "params": {"type": "delay", "timeout": 1.0}},
            ],
            verbose=False,
        )
    parsed = json.loads(result)

    first_log_findings = [f for f in parsed["steps"][0]["findings"] if f["category"] == "logs"]
    second_log_findings = [f for f in parsed["steps"][1]["findings"] if f["category"] == "logs"]
    assert first_log_findings == []
    assert len(second_log_findings) == 1, (
        "a record beyond the first step's fixed end belongs to the next step's window"
    )
    assert second_log_findings[0]["details"]["entry"]["cursor"] == 201
    assert second_log_findings[0]["details"]["entry"]["message"] == "late failure without prefix"
    assert parsed["steps"][0]["log_coverage"]["window_end_cursor"] == 200
    assert parsed["steps"][1]["log_coverage"]["complete"] is True


def test_test_step_baseline_failure_reports_incomplete_coverage_keeping_action_success():
    """A failed count-only watermark must not be mistaken for an empty pre-action buffer."""
    connection = FakeLogWindowConnection([], count_only_error="baseline unavailable")
    mcp = _register_composites(connection)

    result = mcp.tools["test_step"](
        action={"command": "qa.interact", "params": {"key": "E"}},
        assertion={"command": "qa.assert_state", "params": {}},
    )
    parsed = json.loads(result)

    assert parsed["action_success"] is True
    assert parsed["assert_success"] is True
    coverage = parsed["log_coverage"]
    assert coverage["complete"] is False
    assert coverage["cause"] == "log window baseline failed: baseline unavailable"
    assert parsed["finding_count"] == 0
    # Without a watermark no window read is attempted, so nothing is misattributed.
    assert connection.calls == [{"count_only": True, "since_seconds": -1}]


def test_log_reads_use_the_one_shot_transport():
    """Baseline and page log reads use the one-shot transport, never the retrying one."""
    helper_connection = FakeLogWindowConnection(
        [{"cursor": 1, "timestamp": 10.0, "severity": "log", "message": "m1"}],
        watermark=0,
    )
    entries, coverage = _log_window_helper()(helper_connection, 0)
    assert [entry["cursor"] for entry in entries] == [1]
    assert coverage["complete"] is True
    assert helper_connection.retrying_calls == []
    assert len(helper_connection.once_calls) == 1, "one page is read exactly once"

    connection = FakeLogWindowConnection(
        [{"cursor": 1, "timestamp": 10.0, "severity": "log", "message": "m1"}],
        watermark=0,
    )
    mcp = _register_composites(connection)
    parsed = json.loads(
        mcp.tools["test_step"](action={"command": "qa.interact", "params": {}})
    )
    assert parsed["log_coverage"]["complete"] is True
    assert connection.retrying_calls == []
    assert len(connection.once_calls) == 2, "one watermark plus one page, no retry"
    assert connection.once_calls[0].get("count_only") is True
    assert "since_cursor" in connection.once_calls[1]


def test_read_log_window_collects_every_match_once_across_pages():
    """The finite window collector returns each match once; omitted matches are never consumed."""
    records = [
        {"cursor": index, "timestamp": 100.0 + index * 0.01, "severity": "log", "message": f"m{index}"}
        for index in range(1, 251)
    ]
    records[200] = {"cursor": 201, "timestamp": 102.01, "severity": "error", "message": "page-two error"}
    connection = FakeLogWindowConnection(records)

    entries, coverage = _log_window_helper()(connection, 0)

    cursors = [entry["cursor"] for entry in entries]
    assert cursors == sorted(cursors), "entries are returned in cursor order"
    assert len(set(cursors)) == len(cursors), "no match is re-delivered"
    assert cursors == list(range(1, 251)), "no match is skipped across the page boundary"
    assert any(entry["severity"] == "error" for entry in entries), "the final-page error is collected"
    assert _coverage_value(coverage, "complete") is True


def test_read_log_window_reports_loss_and_empty_continuation_never_regresses():
    """Front eviction is explicit, and an exhausted window never moves its cursor back."""
    evicted_connection = FakeLogWindowConnection(
        [
            {"cursor": index, "timestamp": 10.0 + index, "severity": "log", "message": f"m{index}"}
            for index in range(2, 6)
        ],
        evicted_before=1,
    )
    entries, coverage = _log_window_helper()(evicted_connection, -1)
    assert [entry["cursor"] for entry in entries] == [2, 3, 4, 5]
    assert _coverage_value(coverage, "complete") is False, "front eviction must report incomplete coverage"

    connection = FakeLogWindowConnection(
        [
            {"cursor": index, "timestamp": 10.0 + index, "severity": "log", "message": f"m{index}"}
            for index in range(1, 4)
        ]
    )
    first_entries, first_coverage = _log_window_helper()(connection, -1)
    assert [entry["cursor"] for entry in first_entries] == [1, 2, 3]
    assert _coverage_value(first_coverage, "complete") is True

    first_call_count = len(connection.calls)
    second_entries, second_coverage = _log_window_helper()(connection, 3)
    assert second_entries == []
    assert _coverage_value(second_coverage, "complete") is True

    second_calls = connection.calls[first_call_count:]
    window_calls = [call for call in second_calls if not call.get("count_only")]
    assert all(int(call.get("since_cursor", -1)) >= 3 for call in window_calls), (
        "the exhausted window cursor never regresses"
    )


def test_reporter_writes_summary_and_findings(tmp_path):
    bundle = write_report_bundle(
        scenario_name="Door Smoke",
        summary_lines=["# Summary", "- Pass: 1"],
        findings=[{"severity": "MINOR", "summary": "ok"}],
        root=tmp_path,
    )

    assert Path(bundle["summary_path"]).exists()
    assert Path(bundle["findings_path"]).exists()

    summary = Path(bundle["summary_path"]).read_text(encoding="utf-8")
    findings = json.loads(Path(bundle["findings_path"]).read_text(encoding="utf-8"))
    assert "# Summary" in summary
    assert findings[0]["severity"] == "MINOR"
