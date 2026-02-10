"""Unit tests for ResponseCache."""

import time
import unittest
from unittest.mock import patch

from cortex_mcp.cache import ResponseCache


class TestResponseCache(unittest.TestCase):

    def setUp(self):
        self.cache = ResponseCache()

    def test_get_miss_returns_none(self):
        result = self.cache.get("nonexistent")
        self.assertIsNone(result)

    def test_set_and_get(self):
        data = {"success": True, "data": {"tables": []}}
        self.cache.set("cmd:{}", data, ttl=60)
        result = self.cache.get("cmd:{}")
        self.assertEqual(result, data)

    def test_ttl_expiration(self):
        data = {"success": True}
        now = 1000.0

        with patch("cortex_mcp.cache.time") as mock_time:
            mock_time.monotonic.return_value = now
            self.cache.set("key", data, ttl=10)

            # Still valid at t+9
            mock_time.monotonic.return_value = now + 9
            self.assertIsNotNone(self.cache.get("key"))

            # Expired at t+11
            mock_time.monotonic.return_value = now + 11
            self.assertIsNone(self.cache.get("key"))

    def test_invalidate_all(self):
        self.cache.set("a:1", {"a": 1}, ttl=60)
        self.cache.set("b:2", {"b": 2}, ttl=60)
        self.cache.set("c:3", {"c": 3}, ttl=60)

        removed = self.cache.invalidate(None)
        self.assertEqual(removed, 3)
        self.assertIsNone(self.cache.get("a:1"))
        self.assertIsNone(self.cache.get("b:2"))
        self.assertIsNone(self.cache.get("c:3"))

    def test_invalidate_pattern(self):
        self.cache.set("list_datatables:{}", {"tables": []}, ttl=60)
        self.cache.set("list_datatables:{\"path_filter\": \"/Game\"}", {"tables": [1]}, ttl=60)
        self.cache.set("list_gameplay_tags:{}", {"tags": []}, ttl=60)

        removed = self.cache.invalidate("list_datatables:")
        self.assertEqual(removed, 2)
        self.assertIsNone(self.cache.get("list_datatables:{}"))
        # Tags cache should remain
        self.assertIsNotNone(self.cache.get("list_gameplay_tags:{}"))

    def test_make_key_deterministic(self):
        # Same params in different order should produce same key
        key1 = ResponseCache.make_key("cmd", {"b": 2, "a": 1})
        key2 = ResponseCache.make_key("cmd", {"a": 1, "b": 2})
        self.assertEqual(key1, key2)

    def test_make_key_none_params(self):
        key = ResponseCache.make_key("cmd", None)
        self.assertEqual(key, "cmd:{}")

    def test_make_key_different_commands(self):
        key1 = ResponseCache.make_key("list_datatables", {})
        key2 = ResponseCache.make_key("list_gameplay_tags", {})
        self.assertNotEqual(key1, key2)

    def test_stats_tracking(self):
        self.cache.set("key", {"data": 1}, ttl=60)

        # 2 hits
        self.cache.get("key")
        self.cache.get("key")
        # 1 miss
        self.cache.get("missing")

        stats = self.cache.stats
        self.assertEqual(stats["hits"], 2)
        self.assertEqual(stats["misses"], 1)
        self.assertAlmostEqual(stats["hit_rate"], 0.67, places=2)
        self.assertEqual(stats["entries"], 1)

    def test_stats_empty(self):
        stats = self.cache.stats
        self.assertEqual(stats["hits"], 0)
        self.assertEqual(stats["misses"], 0)
        self.assertEqual(stats["hit_rate"], 0.0)
        self.assertEqual(stats["entries"], 0)

    def test_reset_stats(self):
        self.cache.set("key", {"data": 1}, ttl=60)
        self.cache.get("key")
        self.cache.get("missing")
        self.cache.reset_stats()

        stats = self.cache.stats
        self.assertEqual(stats["hits"], 0)
        self.assertEqual(stats["misses"], 0)

    def test_expired_entry_cleaned_up(self):
        now = 1000.0
        with patch("cortex_mcp.cache.time") as mock_time:
            mock_time.monotonic.return_value = now
            self.cache.set("key", {"data": 1}, ttl=5)
            self.assertEqual(self.cache.stats["entries"], 1)

            # After expiry, get removes the entry
            mock_time.monotonic.return_value = now + 6
            self.cache.get("key")
            self.assertEqual(self.cache.stats["entries"], 0)

    def test_overwrite_existing_key(self):
        self.cache.set("key", {"v": 1}, ttl=60)
        self.cache.set("key", {"v": 2}, ttl=60)
        result = self.cache.get("key")
        self.assertEqual(result, {"v": 2})


if __name__ == "__main__":
    unittest.main()
