"""Tests for response size guard and auto-detection."""

import base64
import json
import time
from unittest.mock import MagicMock

import pytest

from cortex_mcp.pagination import PaginationCache, encode_cursor, decode_cursor
from cortex_mcp.response import format_response, _MAX_RESPONSE_CHARS
from cortex_mcp.tools.routers import make_router


def _router_with_mock(domain: str = "data", response_data: dict | None = None):
    """Create a router with a mock connection for testing."""
    connection = MagicMock()
    if response_data is not None:
        connection.send_command.return_value = {"success": True, "data": response_data}
    return make_router(domain, connection, "test docs"), connection


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

        # Should hit the RESPONSE_TOO_LARGE error since the small list isn't truncatable
        assert result.get("_error") == "RESPONSE_TOO_LARGE"

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
        """Response with no lists returns RESPONSE_TOO_LARGE error."""
        data = {"huge_string": "x" * 50_000}
        result = json.loads(format_response(data, "test"))

        assert result["_error"] == "RESPONSE_TOO_LARGE"

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
        """RESPONSE_TOO_LARGE error suggestion should also mention 'limit'."""
        data = {"huge_string": "x" * 50_000}
        result = json.loads(format_response(data, "test"))

        assert "limit" in result["_suggestion"].lower()


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

        response = cache.rebuild_response(key, page, meta)
        assert response["domain"] == "data"
        assert response["command"] == "list"
        assert len(response["rows"]) == 10
        assert response["_pagination"] == meta


@pytest.fixture(autouse=True)
def _clear_pagination_cache():
    """Clear the module-level pagination cache before each test."""
    from cortex_mcp.tools import routers
    routers._pagination_cache.clear()
    yield
    routers._pagination_cache.clear()


class TestRouterPagination:
    """Phase 2B: routers extract limit/cursor and route through pagination."""

    def test_limit_returns_paginated_first_page(self):
        """Passing limit returns a paginated first page."""
        items = [{"id": i} for i in range(100)]
        router, conn = _router_with_mock(response_data={"rows": items, "count": 100})

        result = json.loads(router("list_datatables", {"limit": 20}))

        assert "_pagination" in result
        assert result["_pagination"]["total"] == 100
        assert result["_pagination"]["returned"] == 20
        assert result["_pagination"]["has_more"] is True
        assert len(result["rows"]) == 20
        # limit/cursor should NOT be passed to C++
        call_params = conn.send_command.call_args[0][1]
        assert "limit" not in call_params
        assert "cursor" not in call_params

    def test_cursor_returns_next_page(self):
        """Passing cursor from first page returns the second page."""
        items = [{"id": i} for i in range(100)]
        router, conn = _router_with_mock(response_data={"rows": items, "count": 100})

        # First page
        page1 = json.loads(router("list_datatables", {"limit": 30}))
        cursor = page1["_pagination"]["next_cursor"]

        # Second page — uses cursor, C++ is NOT called again
        conn.send_command.reset_mock()
        page2 = json.loads(router("list_datatables", {"cursor": cursor}))

        conn.send_command.assert_not_called()
        assert page2["_pagination"]["returned"] == 30
        assert page2["rows"][0]["id"] == 30

    def test_last_page_has_no_cursor(self):
        """Last page has has_more=false and next_cursor=null."""
        items = [{"id": i} for i in range(25)]
        router, conn = _router_with_mock(response_data={"rows": items})

        page1 = json.loads(router("list", {"limit": 20}))
        cursor = page1["_pagination"]["next_cursor"]
        page2 = json.loads(router("list", {"cursor": cursor}))

        assert page2["_pagination"]["has_more"] is False
        assert page2["_pagination"]["next_cursor"] is None
        assert len(page2["rows"]) == 5

    def test_expired_cursor_returns_error(self):
        """Expired or invalid cache key returns CURSOR_EXPIRED."""
        router, _ = _router_with_mock()

        fake_cursor = encode_cursor("nonexistent_key", 0, 10)
        result = json.loads(router("list", {"cursor": fake_cursor}))

        assert result["_error"] == "CURSOR_EXPIRED"

    def test_malformed_cursor_returns_error(self):
        """Bad base64 cursor returns INVALID_CURSOR."""
        router, _ = _router_with_mock()

        result = json.loads(router("list", {"cursor": "!!!bad!!!"}))

        assert result["_error"] == "INVALID_CURSOR"

    def test_limit_zero_returns_error(self):
        """limit=0 is rejected."""
        router, _ = _router_with_mock()

        result = json.loads(router("list", {"limit": 0}))

        assert result["_error"] == "INVALID_LIMIT"

    def test_limit_negative_returns_error(self):
        """limit=-1 is rejected."""
        router, _ = _router_with_mock()

        result = json.loads(router("list", {"limit": -1}))

        assert result["_error"] == "INVALID_LIMIT"

    def test_limit_over_200_returns_error(self):
        """limit=999 is rejected."""
        router, _ = _router_with_mock()

        result = json.loads(router("list", {"limit": 999}))

        assert result["_error"] == "INVALID_LIMIT"

    def test_no_limit_no_pagination(self):
        """Without limit, response is returned normally (no pagination metadata)."""
        items = [{"id": i} for i in range(10)]
        router, _ = _router_with_mock(response_data={"rows": items})

        result = json.loads(router("list", {}))

        assert "_pagination" not in result
        assert result["rows"] == items

    def test_limit_on_non_list_response_ignored(self):
        """limit on a command with no qualifying array is a no-op."""
        router, _ = _router_with_mock(response_data={"saved": True, "path": "/Game/Test"})

        result = json.loads(router("save", {"asset_path": "/Game/Test", "limit": 50}))

        assert result["saved"] is True
        assert "_pagination" not in result


class TestPaginationEdgeCases:
    """Edge cases and interaction between Phase 1 and Phase 2."""

    def test_large_limit_triggers_truncation_safety_net(self):
        """A huge page that exceeds 40KB still gets truncated as safety net."""
        # Create items large enough that 200 items exceed 40KB
        items = [{"id": i, "data": "x" * 300} for i in range(500)]
        router, _ = _router_with_mock(response_data={"rows": items})

        result = json.loads(router("list", {"limit": 200}))

        assert "_pagination" in result
        # If the page exceeds 40KB, truncation also kicks in
        result_text = json.dumps(result, indent=2)
        assert len(result_text) <= _MAX_RESPONSE_CHARS
        # Safety net actually triggered — page was truncated below the requested limit
        assert len(result["rows"]) < 200

    def test_cursor_without_limit_uses_embedded_limit(self):
        """cursor without limit in the request still works — limit is in the cursor."""
        items = [{"id": i} for i in range(100)]
        router, conn = _router_with_mock(response_data={"rows": items})

        page1 = json.loads(router("list", {"limit": 25}))
        cursor = page1["_pagination"]["next_cursor"]

        conn.send_command.reset_mock()
        page2 = json.loads(router("list", {"cursor": cursor}))

        assert page2["_pagination"]["returned"] == 25
        assert page2["rows"][0]["id"] == 25

    def test_limit_string_coerced_to_int(self):
        """limit passed as string '50' should work."""
        items = [{"id": i} for i in range(100)]
        router, _ = _router_with_mock(response_data={"rows": items})

        result = json.loads(router("list", {"limit": "50"}))

        assert "_pagination" in result
        assert result["_pagination"]["returned"] == 50

    def test_pagination_metadata_no_count_field(self):
        """Paginated responses use _pagination.total, not a top-level count field."""
        items = [{"id": i} for i in range(100)]
        router, _ = _router_with_mock(response_data={"rows": items, "count": 100})

        result = json.loads(router("list", {"limit": 10}))

        # count from template is preserved, _pagination.total is canonical
        assert result.get("count") == 100
        assert result["_pagination"]["total"] == 100

    def test_lru_promotion_protects_accessed_entry(self):
        """Reading an entry promotes it — the least recently ACCESSED is evicted, not oldest inserted."""
        cache = PaginationCache(max_entries=2, ttl_seconds=60)

        key1 = cache.store("cmd1", {}, "a", [1, 2, 3], {})
        key2 = cache.store("cmd2", {}, "b", [4, 5, 6], {})

        # Access key1 — this promotes it to most recently used
        cache.get_page(key1, offset=0, limit=2)

        # Insert key3 — key2 (least recently accessed) should be evicted
        key3 = cache.store("cmd3", {}, "c", [7, 8, 9], {})

        # key2 was evicted
        with pytest.raises(KeyError, match="CURSOR_EXPIRED"):
            cache.get_page(key2, offset=0, limit=2)

        # key1 and key3 are still available
        page1, _ = cache.get_page(key1, offset=0, limit=2)
        assert page1 == [1, 2]
