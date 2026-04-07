"""Cursor-based pagination cache for large MCP responses."""

import base64
import binascii
import hashlib
import json
import logging
import time
from collections import OrderedDict

logger = logging.getLogger(__name__)


def encode_cursor(key: str, offset: int, limit: int) -> str:
    """Encode pagination state as an opaque base64 cursor token."""
    payload = json.dumps({"key": key, "offset": offset, "limit": limit})
    return base64.b64encode(payload.encode()).decode()


def decode_cursor(cursor: str) -> dict:
    """Decode a cursor token. Raises ValueError with INVALID_CURSOR on failure."""
    try:
        raw = base64.b64decode(cursor)
        data = json.loads(raw)
    except (ValueError, binascii.Error):
        raise ValueError("INVALID_CURSOR")

    if not all(k in data for k in ("key", "offset", "limit")):
        raise ValueError("INVALID_CURSOR")

    return data


def _make_cache_key(qualified_command: str, params: dict) -> str:
    """Deterministic cache key from command + params (excluding limit/cursor)."""
    clean_params = {k: v for k, v in sorted(params.items()) if k not in ("limit", "cursor")}
    raw = json.dumps({"cmd": qualified_command, "params": clean_params}, sort_keys=True)
    return hashlib.sha256(raw.encode()).hexdigest()[:16]


class PaginationCache:
    """LRU + TTL cache for paginated response data."""

    def __init__(self, max_entries: int = 5, ttl_seconds: float = 60.0):
        self._max_entries = max_entries
        self._ttl_seconds = ttl_seconds
        self._entries: OrderedDict[str, tuple[float, str, list, dict]] = OrderedDict()

    def store(
        self,
        qualified_command: str,
        params: dict,
        array_key: str,
        full_list: list,
        response_template: dict,
    ) -> str:
        """Cache a full result list. Returns the cache key."""
        key = _make_cache_key(qualified_command, params)

        # Remove old entry if exists (to update LRU position)
        self._entries.pop(key, None)

        # Evict oldest if at capacity
        while len(self._entries) >= self._max_entries:
            evicted_key, _ = self._entries.popitem(last=False)
            logger.debug("Evicted cache entry %s", evicted_key)

        self._entries[key] = (time.monotonic(), array_key, list(full_list), dict(response_template))
        return key

    def get_page(self, key: str, offset: int, limit: int) -> tuple[list, dict]:
        """Retrieve a page slice. Returns (page_items, pagination_meta).

        Raises KeyError with 'CURSOR_EXPIRED' if entry is missing or expired.
        """
        entry = self._entries.get(key)
        if entry is None:
            raise KeyError("CURSOR_EXPIRED")

        timestamp, array_key, full_list, template = entry
        if time.monotonic() - timestamp > self._ttl_seconds:
            del self._entries[key]
            raise KeyError("CURSOR_EXPIRED")

        # Move to end (most recently used)
        self._entries.move_to_end(key)

        total = len(full_list)
        page = full_list[offset:offset + limit]
        next_offset = offset + len(page)
        has_more = next_offset < total

        meta = {
            "returned": len(page),
            "total": total,
            "has_more": has_more,
            "next_cursor": encode_cursor(key, next_offset, limit) if has_more else None,
        }

        return page, meta

    def clear(self) -> None:
        """Clear all cache entries. Intended for testing."""
        self._entries.clear()

    def rebuild_response(self, key: str, page: list, meta: dict) -> dict:
        """Rebuild a full response dict from template + page + pagination metadata."""
        entry = self._entries.get(key)
        if entry is None:
            raise KeyError("CURSOR_EXPIRED")

        _, array_key, _, template = entry
        response = dict(template)
        response[array_key] = page
        response["_pagination"] = meta
        return response
