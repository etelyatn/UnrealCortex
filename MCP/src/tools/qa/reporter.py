"""QA report generation utility."""

from __future__ import annotations

import json
from datetime import date
from pathlib import Path


def write_report_bundle(
    scenario_name: str,
    summary_lines: list[str],
    findings: list[dict],
    root: str | Path = "QA/reports",
) -> dict:
    """Write summary.md and findings.json for a QA scenario run."""
    safe_name = scenario_name.strip().replace(" ", "-").lower() or "scenario"
    run_dir = Path(root) / f"{date.today().isoformat()}_{safe_name}"
    run_dir.mkdir(parents=True, exist_ok=True)

    summary_path = run_dir / "summary.md"
    findings_path = run_dir / "findings.json"

    summary_path.write_text("\n".join(summary_lines) + "\n", encoding="utf-8")
    findings_path.write_text(json.dumps(findings, indent=2), encoding="utf-8")

    return {
        "report_dir": str(run_dir),
        "summary_path": str(summary_path),
        "findings_path": str(findings_path),
        "finding_count": len(findings),
    }
