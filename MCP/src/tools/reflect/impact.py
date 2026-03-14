"""MCP composite tool for impact analysis before breaking changes."""

import logging
from cortex_mcp.response import format_response
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)

_RISK_MAP = {
    "override": ("high", "Overrides {symbol} - will fail to compile"),
    "call": ("high", "Direct function call - will fail to compile"),
    "read": ("high", "Reads property - will fail to compile"),
    "write": ("high", "Writes property - will fail to compile"),
    "cast": ("medium", "Dynamic cast to class - may return null at runtime"),
    "spawn": ("medium", "Spawns instance of class - may fail if class changes"),
    "component": ("medium", "Has component of this type - may lose properties"),
}

_RISK_ORDER = {"high": 0, "medium": 1, "low": 2}


def _score_risk(
    usages: list[dict], symbol: str, change_type: str = ""
) -> tuple[str, str]:
    """Determine risk level and reason from usage references."""
    if change_type == "removed_class":
        if usages:
            return "high", "Class is being removed - all references will break"
        return "low", "Asset Registry reference only - indirect dependency"

    cast_override = None
    if change_type == "changed_hierarchy":
        cast_override = (
            "high",
            "Hierarchy change - dynamic casts may fail at compile time",
        )

    worst_risk = "low"
    worst_reason = "Asset Registry reference only - indirect dependency"

    for usage in usages:
        ref_type = usage.get("type", "")
        if ref_type == "cast" and cast_override:
            risk, reason = cast_override
        elif ref_type in _RISK_MAP:
            risk, reason = _RISK_MAP[ref_type]
            reason = reason.format(symbol=symbol)
        else:
            continue

        if _RISK_ORDER.get(risk, 99) < _RISK_ORDER.get(worst_risk, 99):
            worst_risk = risk
            worst_reason = reason

    return worst_risk, worst_reason


def _build_recommendation(total: int, by_risk: dict, symbol: str) -> str:
    """Build natural-language recommendation for the AI agent."""
    if total == 0:
        return f"No Blueprints affected by changes to {symbol}."

    parts = [f"{total} Blueprint{'s' if total != 1 else ''} affected."]

    if by_risk.get("high", 0) > 0:
        parts.append(f"{by_risk['high']} high-risk (will fail to compile).")

    if by_risk.get("medium", 0) > 0:
        parts.append(f"{by_risk['medium']} medium-risk (runtime behavior may change).")

    parts.append("Review before proceeding.")
    return " ".join(parts)


def register_reflect_impact_tools(mcp, connection: UEConnection):
    """Register impact analysis composite tools."""

    @mcp.tool()
    def impact_analysis(
        target_class: str,
        symbol: str,
        change_type: str = "removed_function",
        deep_scan: bool = False,
        path_filter: str = "",
        max_blueprints: int = 50,
    ) -> str:
        """Use when you need a complete risk assessment before a breaking change.

        Prefer this over manually calling get_referencers + query_usages because
        it orchestrates discovery and applies risk scoring. For simple asset
        dependency checks, use get_referencers instead.

        Args:
            target_class: The C++ or Blueprint class being changed.
            symbol: The property or function being changed.
            change_type: 'removed_function', 'removed_class', 'changed_property',
                'removed_interface', or 'changed_hierarchy'.
            deep_scan: If true, loads unloaded Blueprints for fuller coverage.
            path_filter: Only analyze Blueprints under this path.
            max_blueprints: Maximum number of Blueprints to scan (default 50).

        Returns:
            Summary + affected Blueprints grouped by risk with usage details,
            and not_scanned metadata if coverage is partial.
        """
        try:
            usages_response = connection.send_command_cached(
                "reflect.find_usages",
                {
                    "symbol": symbol,
                    "class_name": target_class,
                    "scope": "all",
                    "deep_scan": deep_scan,
                    "path_filter": path_filter,
                    "limit": max_blueprints,
                    "max_blueprints": max_blueprints,
                },
                ttl=120,
            )
            usages_data = usages_response.get("data", {})

            affected = []
            by_risk = {"high": 0, "medium": 0, "low": 0}

            for usage in usages_data.get("usages", []):
                references = usage.get("references", [])
                risk, risk_reason = _score_risk(references, symbol, change_type)
                by_risk[risk] = by_risk.get(risk, 0) + 1

                ref_types = sorted({r.get("type", "") for r in references if r.get("type")})

                affected.append(
                    {
                        "blueprint": usage.get("asset_path", ""),
                        "class_name": usage.get("class_name", ""),
                        "risk": risk,
                        "risk_reason": risk_reason,
                        "usages": [
                            {
                                "graph": r.get("context", ""),
                                "node_type": r.get("type", ""),
                                "node_class": r.get("node_class", ""),
                            }
                            for r in references
                        ],
                        "reference_types": ref_types,
                    }
                )

            affected.sort(key=lambda item: _RISK_ORDER.get(item["risk"], 99))

            total_affected = len(affected)
            not_scanned_data = usages_data.get("not_scanned", {})
            not_scanned_count = int(not_scanned_data.get("count", 0) or 0)
            recommendation = _build_recommendation(total_affected, by_risk, symbol)
            scan_coverage = "complete" if not_scanned_count == 0 else "partial"

            result = {
                "summary": {
                    "target": f"{target_class}::{symbol}",
                    "change_type": change_type,
                    "total_affected": total_affected,
                    "by_risk": by_risk,
                    "scan_coverage": scan_coverage,
                    "blueprints_scanned": usages_data.get("blueprints_scanned", 0),
                    "unscanned_count": not_scanned_count,
                    "recommendation": recommendation,
                },
                "affected": affected,
            }

            if not_scanned_count > 0:
                not_scanned_paths = not_scanned_data.get("paths", [])
                result["not_scanned"] = {
                    "count": not_scanned_count,
                    "reason": "Blueprints not loaded. Use deep_scan=true for full coverage.",
                    "paths": not_scanned_paths[:10],
                    "paths_truncated": len(not_scanned_paths) > 10,
                }

            return format_response(result, "impact_analysis")
        except (ConnectionError, RuntimeError) as e:
            return f"Error: {e}"
