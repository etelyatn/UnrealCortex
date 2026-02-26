"""Layer 1 structural QA issue detection."""

from __future__ import annotations


def _get_vector_z(value) -> float | None:
    """Extract Z from either [x,y,z] or {x,y,z}."""
    if isinstance(value, dict):
        z_value = value.get("z")
        return float(z_value) if isinstance(z_value, (int, float)) else None
    if isinstance(value, list) and len(value) == 3:
        z_value = value[2]
        return float(z_value) if isinstance(z_value, (int, float)) else None
    return None


def _get_vector_components(value) -> tuple[float, float, float] | None:
    """Extract (x, y, z) from either [x,y,z] or {x,y,z}."""
    if isinstance(value, dict):
        x = value.get("x")
        y = value.get("y")
        z = value.get("z")
        if all(isinstance(component, (int, float)) for component in (x, y, z)):
            return float(x), float(y), float(z)
        return None
    if isinstance(value, list) and len(value) == 3:
        if all(isinstance(component, (int, float)) for component in value):
            return float(value[0]), float(value[1]), float(value[2])
    return None


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
    location_z = _get_vector_z(location)
    if location_z is not None and location_z < kill_z:
        findings.append(
            {
                "severity": "CRITICAL",
                "category": "physics",
                "summary": "Player fell below kill Z threshold",
                "details": {"z": location_z, "kill_z": kill_z},
            }
        )

    for actor in state.get("nearby_actors", []):
        actor_loc = actor.get("location")
        components = _get_vector_components(actor_loc)
        if components is None:
            continue
        near_origin = abs(components[0]) <= 1.0 and abs(components[1]) <= 1.0 and abs(components[2]) <= 1.0
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
