"""Layer 1 structural QA issue detection."""

from __future__ import annotations


def detect_structural_issues(
    observed_state: dict | None,
    wait_result: dict | None = None,
    recent_logs: dict | None = None,
    kill_z: float = -10000.0,
    min_fps: float = 20.0,
) -> list[dict]:
    """Return a list of rule-based findings from QA step artifacts."""
    findings: list[dict] = []
    state = observed_state or {}
    player = state.get("player", {})

    location = player.get("location")
    if isinstance(location, list) and len(location) == 3 and location[2] < kill_z:
        findings.append(
            {
                "severity": "CRITICAL",
                "category": "physics",
                "summary": "Player fell below kill Z threshold",
                "details": {"z": location[2], "kill_z": kill_z},
            }
        )

    for actor in state.get("nearby_actors", []):
        actor_loc = actor.get("location")
        if not (isinstance(actor_loc, list) and len(actor_loc) == 3):
            continue
        near_origin = abs(actor_loc[0]) <= 1.0 and abs(actor_loc[1]) <= 1.0 and abs(actor_loc[2]) <= 1.0
        if near_origin:
            findings.append(
                {
                    "severity": "MINOR",
                    "category": "placement",
                    "summary": "Actor near world origin",
                    "details": {"actor": actor.get("name", ""), "location": actor_loc},
                }
            )

    if isinstance(wait_result, dict) and wait_result.get("timed_out"):
        findings.append(
            {
                "severity": "MAJOR",
                "category": "timeout",
                "summary": "wait_for condition timed out",
                "details": wait_result,
            }
        )

    fps = state.get("game_state", {}).get("fps")
    if isinstance(fps, (int, float)) and fps < min_fps:
        findings.append(
            {
                "severity": "MAJOR",
                "category": "performance",
                "summary": "Frame rate below threshold",
                "details": {"fps": fps, "min_fps": min_fps},
            }
        )

    logs = (recent_logs or {}).get("logs", [])
    for entry in logs:
        text = entry.get("message", "") if isinstance(entry, dict) else str(entry)
        level = entry.get("level", "") if isinstance(entry, dict) else ""
        if level.upper() == "ERROR" or "Error:" in text:
            findings.append(
                {
                    "severity": "MAJOR",
                    "category": "logs",
                    "summary": "Error log entry detected",
                    "details": {"entry": entry},
                }
            )

    return findings
