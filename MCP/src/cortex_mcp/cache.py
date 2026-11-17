"""In-memory TTL cache for MCP server responses."""

import json
import logging
import threading
import time

logger = logging.getLogger(__name__)


class ResponseCache:
    """TTL-based in-memory cache for UE command responses.

    Uses time.monotonic() for TTL (immune to clock changes).
    Cache key: f"{command}:{json.dumps(params, sort_keys=True)}"
    Tool calls run on worker threads, so every store access holds one lock.
    """

    def __init__(self):
        self._store: dict[str, tuple[float, dict]] = {}  # key -> (expires_at, value)
        self._lock = threading.Lock()
        self._hits = 0
        self._misses = 0
        # Bumped by every invalidation. A read captures it before dispatching so a write that
        # completes while the fetch is in flight prevents that pre-write response from being
        # cached after the invalidation.
        self._generation = 0

    @property
    def generation(self) -> int:
        """Invalidation generation; changes whenever the cache may no longer be current."""
        return self._generation

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
        with self._lock:
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

    def set(
        self,
        key: str,
        value: dict,
        ttl: float,
        *,
        expected_generation: int | None = None,
    ) -> bool:
        """Store a value with TTL in seconds.

        With ``expected_generation``, skip the store when the cache was invalidated after the
        caller started its fetch: that response predates the write and must not be served.
        Returns whether the value was stored.
        """
        with self._lock:
            if expected_generation is not None and expected_generation != self._generation:
                logger.debug("Cache SKIP: %s (invalidated during fetch)", key)
                return False
            self._store[key] = (time.monotonic() + ttl, value)
            logger.debug("Cache SET: %s (ttl=%.0fs)", key, ttl)
            return True

    def invalidate(self, pattern: str | None) -> int:
        """Invalidate cache entries matching a pattern prefix, or all if pattern is None.

        Returns the number of entries removed.
        """
        with self._lock:
            self._generation += 1
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
