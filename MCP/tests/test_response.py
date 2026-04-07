"""Tests for response size guard and auto-detection."""

import json

from cortex_mcp.response import format_response, _MAX_RESPONSE_CHARS


def _make_large_list(key: str, count: int = 500) -> dict:
    """Build a response with a large list that exceeds _MAX_RESPONSE_CHARS."""
    return {key: [{"id": i, "name": f"Item_{i}", "path": f"/Game/Test/Asset_{i}"} for i in range(count)]}


class TestAutoDetectTruncation:
    """Phase 1: auto-detect truncatable arrays."""

    def test_unknown_array_key_is_truncated(self):
        """Array keys NOT in the old allowlist must still be truncated."""
        data = _make_large_list("blueprints", 500)
        result = json.loads(format_response(data, "test"))

        assert "_truncated" in result
        assert result["_truncated"]["original_count"] == 500
        assert len(result["blueprints"]) == result["_truncated"]["returned_count"]
        assert len(json.dumps(result, indent=2)) <= _MAX_RESPONSE_CHARS

    def test_known_array_key_still_works(self):
        """Previously allowlisted keys like 'rows' must still be truncated."""
        data = _make_large_list("rows", 500)
        result = json.loads(format_response(data, "test"))

        assert "_truncated" in result
        assert result["_truncated"]["original_count"] == 500

    def test_small_list_not_truncated(self):
        """Lists with fewer than 10 items must NOT be truncated even if response is large."""
        # Build a response that's large due to non-list data, with a small list
        data = {
            "details": "x" * 50_000,
            "items": [{"id": i} for i in range(5)],
        }
        result = json.loads(format_response(data, "test"))

        # Should hit the response_too_large error since the small list isn't truncatable
        assert result.get("_error") == "response_too_large"

    def test_multiple_large_lists_truncates_largest(self):
        """When multiple lists qualify, the largest one is truncated."""
        data = {
            "small_list": [{"id": i} for i in range(15)],
            "big_list": [{"id": i, "name": f"Item_{i}", "path": f"/Game/Test/Asset_{i}"} for i in range(500)],
        }
        result = json.loads(format_response(data, "test"))

        assert "_truncated" in result
        assert result["_truncated"]["original_count"] == 500
        # big_list was truncated, small_list untouched
        assert len(result["small_list"]) == 15

    def test_no_lists_returns_error(self):
        """Response with no lists returns response_too_large error."""
        data = {"huge_string": "x" * 50_000}
        result = json.loads(format_response(data, "test"))

        assert result["_error"] == "response_too_large"

    def test_small_response_passes_through(self):
        """Responses under 40KB are returned unchanged."""
        data = {"items": [1, 2, 3], "count": 3}
        result = json.loads(format_response(data, "test"))

        assert result == data

    def test_truncation_suggestion_mentions_limit(self):
        """Truncation metadata suggests using 'limit' parameter."""
        data = _make_large_list("results", 500)
        result = json.loads(format_response(data, "test"))

        assert "limit" in result["_truncated"]["suggestion"].lower()

    def test_no_truncatable_list_error_suggestion_mentions_limit(self):
        """response_too_large error suggestion should also mention 'limit'."""
        data = {"huge_string": "x" * 50_000}
        result = json.loads(format_response(data, "test"))

        assert "limit" in result["_suggestion"].lower()
