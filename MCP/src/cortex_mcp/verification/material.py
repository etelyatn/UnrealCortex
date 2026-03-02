"""Material verification helpers."""

from __future__ import annotations

from collections import Counter

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

    expected_class_counts = Counter(
        _normalize_class(node.get("class", "")) for node in spec_nodes if node.get("class", "")
    )
    actual_class_counts = Counter(
        node.get("expression_class", "") for node in readback.get("nodes", []) if node.get("expression_class", "")
    )
    for full_class, expected_count in expected_class_counts.items():
        short_class = full_class[len(_CLASS_PREFIX):] if full_class.startswith(_CLASS_PREFIX) else full_class
        name, check = check_gte(
            f"node_count:{short_class}",
            expected_count,
            actual_class_counts.get(full_class, 0),
        )
        checks[name] = check

    # blend_mode and shading_model are exposed directly by get_material; check with equality.
    # All other material_properties keys are verified for presence only — get_material may
    # not expose every settable property in its response.
    for prop_key, prop_value in spec_props.items():
        if prop_key == "blend_mode":
            name, check = check_eq("blend_mode", prop_value, readback.get("blend_mode"))
        elif prop_key == "shading_model":
            name, check = check_eq("shading_model", prop_value, readback.get("shading_model"))
        else:
            name, check = check_exists(f"property:{prop_key}", prop_key in readback)
        checks[name] = check

    verified = all(check.passed for check in checks.values())
    result = VerificationResult(verified=verified, checks=checks)
    if not verified:
        result.error_code = "VERIFICATION_FAILED"
    return result
