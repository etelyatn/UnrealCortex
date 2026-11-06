"""Cursor-based pagination cache for large MCP responses."""

import base64
import binascii
import hashlib
import json
import logging
import time
from collections import OrderedDict

logger = logging.getLogger(__name__)


def encode_cursor(
    key: str,
    offset: int,
    limit: int,
    *,
    command: str | None = None,
    array: str | None = None,
    snapshot_id: int | None = None,
) -> str:
    """Encode pagination state as an opaque base64 cursor token.

    ``command``/``array`` record the owning read's provenance so a cursor cannot be replayed
    against an unrelated read; callers that still build a bare cursor keep working.
    """
    payload: dict = {"key": key, "offset": offset, "limit": limit}
    if snapshot_id is not None:
        payload["snapshot_id"] = snapshot_id
    if command is not None:
        payload["command"] = command
    if array is not None:
        payload["array"] = array
    return base64.b64encode(json.dumps(payload).encode()).decode()


def decode_cursor(cursor: str) -> dict:
    """Decode a cursor token. Raises ValueError with INVALID_CURSOR on failure.

    A cursor must carry a string key and integer offsets, so a forged token refuses here instead
    of failing later while slicing the cached list.
    """
    try:
        raw = base64.b64decode(cursor)
        data = json.loads(raw)
    except (ValueError, binascii.Error):
        raise ValueError("INVALID_CURSOR")

    if not isinstance(data, dict) or not all(k in data for k in ("key", "offset", "limit")):
        raise ValueError("INVALID_CURSOR")

    if not isinstance(data["key"], str) or not isinstance(data["offset"], int) or not isinstance(data["limit"], int):
        raise ValueError("INVALID_CURSOR")
    snapshot_id = data.get("snapshot_id")
    if snapshot_id is not None and (isinstance(snapshot_id, bool) or not isinstance(snapshot_id, int)):
        raise ValueError("INVALID_CURSOR")

    return data


def _make_cache_key(qualified_command: str, params: dict) -> str:
    """Deterministic cache key from command + params (excluding limit/cursor)."""
    clean_params = {k: v for k, v in sorted(params.items()) if k not in ("limit", "cursor")}
    raw = json.dumps({"cmd": qualified_command, "params": clean_params}, sort_keys=True)
    return hashlib.sha256(raw.encode()).hexdigest()[:16]


class PaginationCache:
    """LRU + TTL cache of paginated response snapshots.

    Every store mints a monotonically increasing snapshot id that is part of the cache key, so
    identical requests never overwrite each other's snapshot: a cursor always addresses the exact
    result set it was minted from, or expires.
    """

    def __init__(self, max_entries: int = 16, ttl_seconds: float = 600.0):
        self._max_entries = max_entries
        self._ttl_seconds = ttl_seconds
        self._next_snapshot_id = 1
        # (stored_at, qualified_command, array_key, full_list, response_template, snapshot_id)
        self._entries: OrderedDict[str, tuple[float, str, str, list, dict, int]] = OrderedDict()

    def store(
        self,
        qualified_command: str,
        params: dict,
        array_key: str,
        full_list: list,
        response_template: dict,
    ) -> str:
        """Cache a full result list as a new snapshot. Returns the snapshot's cache key."""
        snapshot_id = self._next_snapshot_id
        self._next_snapshot_id += 1
        key = f"{_make_cache_key(qualified_command, params)}-{snapshot_id}"

        # Evict least recently used snapshots when at capacity
        while len(self._entries) >= self._max_entries:
            evicted_key, _ = self._entries.popitem(last=False)
            logger.debug("Evicted cache entry %s", evicted_key)

        self._entries[key] = (
            time.monotonic(),
            qualified_command,
            array_key,
            list(full_list),
            dict(response_template),
            snapshot_id,
        )
        return key

    def snapshot_id(self, key: str) -> int:
        """Snapshot id of a live entry. Raises KeyError('CURSOR_EXPIRED') when missing/expired."""
        entry = self._live_entry(key)
        if entry is None:
            raise KeyError("CURSOR_EXPIRED")
        return entry[5]

    def _live_entry(self, key: str):
        entry = self._entries.get(key)
        if entry is None:
            return None
        if time.monotonic() - entry[0] > self._ttl_seconds:
            del self._entries[key]
            return None
        return entry

    def get_page(self, key: str, offset: int, limit: int) -> tuple[list, dict]:
        """Retrieve a page slice. Returns (page_items, pagination_meta).

        Raises KeyError with 'CURSOR_EXPIRED' if entry is missing or expired.
        """
        entry = self._live_entry(key)
        if entry is None:
            raise KeyError("CURSOR_EXPIRED")

        _timestamp, qualified_command, array_key, full_list, _template, snapshot_id = entry

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
            "snapshot_id": snapshot_id,
            "next_cursor": (
                encode_cursor(key, next_offset, limit, snapshot_id=snapshot_id)
                if has_more
                else None
            ),
        }

        return page, meta

    def entry_provenance(self, key: str) -> tuple[str, str, int] | None:
        """Owner command, primary array and snapshot id of a live entry, or None when missing/expired.

        Used to validate a cursor's self-declared provenance against the snapshot it addresses
        before any value is sliced or rebuilt.
        """
        entry = self._live_entry(key)
        if entry is None:
            return None
        _timestamp, qualified_command, array_key, _full_list, _template, snapshot_id = entry
        return qualified_command, array_key, snapshot_id

    def clear(self) -> None:
        """Clear all cache entries. Intended for testing."""
        self._entries.clear()

    def rebuild_response(self, key: str, page: list, meta: dict) -> dict:
        """Rebuild a full response dict from template + page + pagination metadata."""
        entry = self._live_entry(key)
        if entry is None:
            raise KeyError("CURSOR_EXPIRED")

        _, _qualified_command, array_key, _, template, _snapshot_id = entry
        response = dict(template)
        response[array_key] = page
        response["_pagination"] = meta
        return response
