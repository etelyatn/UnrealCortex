"""QA helpers shared by consolidated MCP tools."""

from .detector import detect_structural_issues
from .reporter import write_report_bundle

__all__ = ["detect_structural_issues", "write_report_bundle"]
