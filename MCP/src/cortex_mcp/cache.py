"""In-memory TTL cache for MCP server responses."""

import json
import logging
import time

logger = logging.getLogger(__name__)


class ResponseCache:
    """TTL-based in-memory cache for UE command responses.

    Uses time.monotonic() for TTL (immune to clock changes).
    Cache key: f"{command}:{json.dumps(params, sort_keys=True)}"
    """

    def __init__(self):
        self._store: dict[str, tuple[float, dict]] = {}  # key -> (expires_at, value)
        self._hits = 0
        self._misses = 0

    @property
    def stats(self) -> dict:
        """Return cache hit/miss statistics."""
        total = self._hits + self._misses
        return {
            "hits": self._hits,
            "misses": self._misses,
            "hit_rate": round(self._hits / total, 2) if total > 0 else 0.0,
            "entries": len(self._store),
        }

    @staticmethod
    def make_key(command: str, params: dict | None) -> str:
        """Build a deterministic cache key from command + params."""
        param_str = json.dumps(params or {}, sort_keys=True)
        return f"{command}:{param_str}"

    def get(self, key: str) -> dict | None:
        """Get a cached value if it exists and hasn't expired."""
        entry = self._store.get(key)
        if entry is None:
            self._misses += 1
            logger.debug("Cache MISS: %s", key)
            return None

        expires_at, value = entry
        if time.monotonic() > expires_at:
            del self._store[key]
            self._misses += 1
            logger.debug("Cache EXPIRED: %s", key)
            return None

        self._hits += 1
        logger.debug("Cache HIT: %s", key)
        return value

    def set(self, key: str, value: dict, ttl: float) -> None:
        """Store a value with TTL in seconds."""
        self._store[key] = (time.monotonic() + ttl, value)
        logger.debug("Cache SET: %s (ttl=%.0fs)", key, ttl)

    def invalidate(self, pattern: str | None) -> int:
        """Invalidate cache entries matching a pattern prefix, or all if pattern is None.

        Returns the number of entries removed.
        """
        if pattern is None:
            count = len(self._store)
            self._store.clear()
            logger.debug("Cache CLEAR ALL: removed %d entries", count)
            return count

        to_remove = [k for k in self._store if k.startswith(pattern)]
        for k in to_remove:
            del self._store[k]
        logger.debug("Cache INVALIDATE '%s': removed %d entries", pattern, len(to_remove))
        return len(to_remove)

    def reset_stats(self) -> None:
        """Reset hit/miss counters."""
        self._hits = 0
        self._misses = 0
