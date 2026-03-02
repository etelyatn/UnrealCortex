"""Material verification helpers."""

from __future__ import annotations

from . import CheckResult, VerificationResult, check_eq, check_exists, check_gte

_CLASS_PREFIX = "MaterialExpression"


def _normalize_class(name: str) -> str:
    if name.startswith(_CLASS_PREFIX):
        return name
    return f"{_CLASS_PREFIX}{name}"


def verify_material(spec: dict, readback: dict) -> VerificationResult:
    checks: dict[str, CheckResult] = {}
    spec_nodes = spec.get("nodes", [])
    spec_connections = spec.get("connections", [])
    spec_props = spec.get("material_properties", {})

    expected_node_count = len(spec_nodes) + 1
    actual_node_count = readback.get("node_count", 0)
    name, check = check_gte("node_count", expected_node_count, actual_node_count)
    checks[name] = check

    if spec_connections:
        name, check = check_gte(
            "connection_count",
            len(spec_connections),
            len(readback.get("connections", [])),
        )
        checks[name] = check

    readback_classes = {node.get("expression_class", "") for node in readback.get("nodes", [])}
    for node in spec_nodes:
        node_class = node.get("class", "")
        full_class = _normalize_class(node_class)
        name, check = check_exists(f"node_exists:{node_class}", full_class in readback_classes)
        checks[name] = check

    if "blend_mode" in spec_props:
        name, check = check_eq("blend_mode", spec_props["blend_mode"], readback.get("blend_mode"))
        checks[name] = check

    if "shading_model" in spec_props:
        name, check = check_eq(
            "shading_model",
            spec_props["shading_model"],
            readback.get("shading_model"),
        )
        checks[name] = check

    verified = all(check.passed for check in checks.values())
    result = VerificationResult(verified=verified, checks=checks)
    if not verified:
        result.error_code = "VERIFICATION_FAILED"
    return result
