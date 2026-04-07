"""Tests for response size guard and auto-detection."""

import base64
import json
import time

import pytest

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


import base64
import time

from cortex_mcp.pagination import PaginationCache, encode_cursor, decode_cursor


class TestCursorEncoding:
    """Cursor encode/decode round-trip."""

    def test_encode_decode_round_trip(self):
        cursor = encode_cursor("abc123", offset=50, limit=50)
        decoded = decode_cursor(cursor)

        assert decoded["key"] == "abc123"
        assert decoded["offset"] == 50
        assert decoded["limit"] == 50

    def test_decode_malformed_base64_raises(self):
        with pytest.raises(ValueError, match="INVALID_CURSOR"):
            decode_cursor("not-valid-base64!!!")

    def test_decode_invalid_json_raises(self):
        bad = base64.b64encode(b"not json").decode()
        with pytest.raises(ValueError, match="INVALID_CURSOR"):
            decode_cursor(bad)

    def test_decode_missing_fields_raises(self):
        bad = base64.b64encode(json.dumps({"key": "x"}).encode()).decode()
        with pytest.raises(ValueError, match="INVALID_CURSOR"):
            decode_cursor(bad)


class TestPaginationCache:
    """Cache storage, retrieval, eviction, TTL."""

    def test_store_and_retrieve_first_page(self):
        cache = PaginationCache(max_entries=5, ttl_seconds=60)
        full_list = [{"id": i} for i in range(100)]
        response_template = {"count": 100}

        key = cache.store("data.list_datatables", {}, "rows", full_list, response_template)
        page, meta = cache.get_page(key, offset=0, limit=20)

        assert len(page) == 20
        assert page[0] == {"id": 0}
        assert meta["total"] == 100
        assert meta["returned"] == 20
        assert meta["has_more"] is True
        assert meta["next_cursor"] is not None

    def test_retrieve_last_page(self):
        cache = PaginationCache(max_entries=5, ttl_seconds=60)
        full_list = [{"id": i} for i in range(100)]

        key = cache.store("data.list", {}, "rows", full_list, {})
        page, meta = cache.get_page(key, offset=90, limit=20)

        assert len(page) == 10
        assert meta["returned"] == 10
        assert meta["has_more"] is False
        assert meta["next_cursor"] is None

    def test_expired_entry_raises(self):
        cache = PaginationCache(max_entries=5, ttl_seconds=0.01)
        full_list = [{"id": i} for i in range(10)]

        key = cache.store("cmd", {}, "items", full_list, {})
        time.sleep(0.05)

        with pytest.raises(KeyError, match="CURSOR_EXPIRED"):
            cache.get_page(key, offset=0, limit=5)

    def test_missing_key_raises(self):
        cache = PaginationCache(max_entries=5, ttl_seconds=60)

        with pytest.raises(KeyError, match="CURSOR_EXPIRED"):
            cache.get_page("nonexistent", offset=0, limit=5)

    def test_lru_eviction(self):
        cache = PaginationCache(max_entries=2, ttl_seconds=60)

        key1 = cache.store("cmd1", {}, "a", [1, 2, 3], {})
        key2 = cache.store("cmd2", {}, "b", [4, 5, 6], {})
        key3 = cache.store("cmd3", {}, "c", [7, 8, 9], {})

        # key1 was evicted (LRU, oldest)
        with pytest.raises(KeyError, match="CURSOR_EXPIRED"):
            cache.get_page(key1, offset=0, limit=2)

        # key2 and key3 still available
        page2, _ = cache.get_page(key2, offset=0, limit=2)
        assert page2 == [4, 5]

    def test_same_command_reuses_cache_key(self):
        cache = PaginationCache(max_entries=5, ttl_seconds=60)

        key1 = cache.store("data.list", {"type": "Blueprint"}, "items", [1, 2], {})
        key2 = cache.store("data.list", {"type": "Blueprint"}, "items", [3, 4], {})

        assert key1 == key2
        # Second store overwrites — returns fresh data
        page, _ = cache.get_page(key2, offset=0, limit=10)
        assert page == [3, 4]

    def test_cache_key_excludes_limit_and_cursor(self):
        cache = PaginationCache(max_entries=5, ttl_seconds=60)

        key1 = cache.store("data.list", {"type": "BP"}, "items", [1], {})
        key2 = cache.store("data.list", {"type": "BP", "limit": 50, "cursor": "abc"}, "items", [2], {})

        assert key1 == key2

    def test_rebuild_response(self):
        cache = PaginationCache(max_entries=5, ttl_seconds=60)
        template = {"domain": "data", "command": "list"}
        full_list = [{"id": i} for i in range(50)]

        key = cache.store("data.list", {}, "rows", full_list, template)
        page, meta = cache.get_page(key, offset=0, limit=10)

        response = cache.rebuild_response(key, "rows", page, meta)
        assert response["domain"] == "data"
        assert response["command"] == "list"
        assert len(response["rows"]) == 10
        assert response["_pagination"] == meta
