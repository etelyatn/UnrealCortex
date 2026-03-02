"""UMG verification helpers."""

from __future__ import annotations

from . import CheckResult, VerificationResult, check_eq, check_exists, check_gte


def _flatten_spec_widgets(widgets: list[dict]) -> list[dict]:
    flat: list[dict] = []
    for widget in widgets:
        flat.append(widget)
        flat.extend(_flatten_spec_widgets(widget.get("children", [])))
    return flat


def _flatten_tree(root: dict) -> list[dict]:
    if not root:
        return []
    flat: list[dict] = []

    def _walk(node: dict) -> None:
        flat.append(node)
        for child in node.get("children", []):
            _walk(child)

    _walk(root)
    return flat


def verify_umg(spec: dict, readback: dict) -> VerificationResult:
    checks: dict[str, CheckResult] = {}

    spec_widgets = _flatten_spec_widgets(spec.get("widgets", []))
    readback_widgets = _flatten_tree(readback.get("tree", {}))

    name, check = check_gte("widget_count", len(spec_widgets), len(readback_widgets))
    checks[name] = check

    readback_by_name = {widget.get("name"): widget for widget in readback_widgets if widget.get("name")}

    for widget in spec_widgets:
        widget_name = widget.get("name", "")
        expected_class = widget.get("class", "")
        actual_widget = readback_by_name.get(widget_name)

        exists_name, exists_check = check_exists(f"widget_exists:{widget_name}", actual_widget is not None)
        checks[exists_name] = exists_check
        if actual_widget is None:
            continue

        class_name, class_check = check_eq(
            f"widget_class:{widget_name}",
            expected_class,
            actual_widget.get("class"),
        )
        checks[class_name] = class_check

    verified = all(check.passed for check in checks.values())
    result = VerificationResult(verified=verified, checks=checks)
    if not verified:
        result.error_code = "VERIFICATION_FAILED"
    return result
