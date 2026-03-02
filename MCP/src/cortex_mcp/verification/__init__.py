"""Composite tool verification helpers and result types."""

from __future__ import annotations

import dataclasses
from typing import Any

VERIFICATION_TIMEOUT = 5.0


@dataclasses.dataclass
class CheckResult:
    """Single verification check outcome."""

    expected: Any
    actual: Any
    _gte: bool = dataclasses.field(default=False, repr=False, compare=False)

    @property
    def passed(self) -> bool:
        if self._gte:
            return self.actual >= self.expected
        return self.expected == self.actual

    def to_dict(self) -> dict[str, Any]:
        return {
            "expected": self.expected,
            "actual": self.actual,
            "pass": self.passed,
        }


@dataclasses.dataclass
class VerificationResult:
    """Aggregate verification outcome for a composite command."""

    verified: bool | None
    checks: dict[str, CheckResult] = dataclasses.field(default_factory=dict)
    error_code: str | None = None
    error: str | None = None
    skipped: list[str] | None = None
    message: str | None = None

    def to_dict(self) -> dict[str, Any]:
        payload: dict[str, Any] = {"verified": self.verified}
        if self.error_code:
            payload["error_code"] = self.error_code
        if self.error:
            payload["error"] = self.error
        if self.checks:
            payload["checks"] = {name: check.to_dict() for name, check in self.checks.items()}
        if self.skipped:
            payload["skipped"] = self.skipped
        if self.message:
            payload["message"] = self.message
        return payload


def check_gte(name: str, expected: int, actual: int) -> tuple[str, CheckResult]:
    return name, CheckResult(expected=expected, actual=actual, _gte=True)


def check_eq(name: str, expected: Any, actual: Any) -> tuple[str, CheckResult]:
    return name, CheckResult(expected=expected, actual=actual)


def check_exists(name: str, found: bool) -> tuple[str, CheckResult]:
    return name, CheckResult(expected=True, actual=found)
