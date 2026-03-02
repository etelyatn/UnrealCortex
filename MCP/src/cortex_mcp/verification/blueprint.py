"""Blueprint verification helpers."""

from __future__ import annotations

from collections import Counter

from . import CheckResult, VerificationResult, check_exists, check_gte

_CLASS_MAP = {
    "Event": "K2Node_Event",
    "CustomEvent": "K2Node_CustomEvent",
    "CallFunction": "K2Node_CallFunction",
    "Branch": "K2Node_IfThenElse",
    "IfThenElse": "K2Node_IfThenElse",
    "Sequence": "K2Node_ExecutionSequence",
    "VariableGet": "K2Node_VariableGet",
    "VariableSet": "K2Node_VariableSet",
}


def _normalize_node_class(class_name: str) -> str:
    if class_name.startswith("K2Node_"):
        return class_name
    return _CLASS_MAP.get(class_name, class_name)


def verify_blueprint(spec: dict, readback: dict) -> VerificationResult:
    checks: dict[str, CheckResult] = {}
    info = readback.get("info", {})
    spec_variables = spec.get("variables", [])
    spec_functions = spec.get("functions", [])
    spec_nodes = spec.get("nodes", [])
    graph_name = spec.get("graph_name", "EventGraph")

    is_compiled = bool(info.get("is_compiled", False))
    if not is_compiled:
        return VerificationResult(
            verified=False,
            error_code="COMPILE_FAILED",
            error="Blueprint is not compiled after composite execution",
            checks=checks,
        )

    name, check = check_gte("variables_count", len(spec_variables), len(info.get("variables", [])))
    checks[name] = check

    name, check = check_gte("functions_count", len(spec_functions), len(info.get("functions", [])))
    checks[name] = check

    graph_info = None
    for graph in info.get("graphs", []):
        if graph.get("name") == graph_name:
            graph_info = graph
            break

    name, check = check_exists("graph_exists", graph_info is not None)
    checks[name] = check

    if graph_info is not None:
        name, check = check_gte("graph_node_count", len(spec_nodes), graph_info.get("node_count", 0))
        checks[name] = check

    expected_class_counts = Counter(
        _normalize_node_class(node.get("class", "")) for node in spec_nodes if node.get("class", "")
    )
    actual_class_counts = Counter(
        _normalize_node_class(node.get("class", "")) for node in readback.get("nodes", []) if node.get("class", "")
    )
    for node in spec_nodes:
        normalized = _normalize_node_class(node.get("class", ""))
        if not normalized:
            continue
        short_name = node.get("class", "")
        check_name = f"node_count:{short_name}"
        if check_name in checks:
            continue
        name, check = check_gte(
            check_name,
            expected_class_counts.get(normalized, 0),
            actual_class_counts.get(normalized, 0),
        )
        checks[name] = check

    verified = all(check.passed for check in checks.values())
    result = VerificationResult(verified=verified, checks=checks)
    if not verified:
        result.error_code = "VERIFICATION_FAILED"
    return result
