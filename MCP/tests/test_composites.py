"""Tests for _resolve_class_name and _BP_CLASS_MAP correctness."""

import sys
from pathlib import Path

import pytest

# Add tools directory to path for imports
tools_dir = Path(__file__).parent.parent / "tools"
sys.path.insert(0, str(tools_dir))

from blueprint.composites import _resolve_class_name, _BP_CLASS_MAP


def test_resolve_known_short_name():
    assert _resolve_class_name("Event") == "UK2Node_Event"


def test_resolve_full_class_passthrough():
    assert _resolve_class_name("UK2Node_CallFunction") == "UK2Node_CallFunction"


def test_resolve_unknown_raises_error():
    with pytest.raises(ValueError) as exc_info:
        _resolve_class_name("UnknownNode")
    assert "Unknown node class 'UnknownNode'" in str(exc_info.value)
    assert "Known short names:" in str(exc_info.value)


def test_function_entry_not_in_class_map():
    assert "FunctionEntry" not in _BP_CLASS_MAP


def test_function_result_not_in_class_map():
    assert "FunctionResult" not in _BP_CLASS_MAP


def test_foreach_loop_not_in_class_map():
    assert "ForEachLoop" not in _BP_CLASS_MAP
