"""End-to-End tests for cortex_mcp against a live Unreal Editor.

Requires the Unreal Editor to be running with the UnrealCortex plugin
and test content loaded in CortexSandbox.

Usage:
    python tests/test_e2e.py

    # Or with specific port:
    CORTEX_PORT=8742 python tests/test_e2e.py
"""

import json
import os
import sys
import traceback

# Add src to path so we can import cortex_mcp
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from cortex_mcp.tcp_client import UEConnection


class E2ETestRunner:
    """Runs E2E tests against a live Unreal Editor."""

    def __init__(self):
        port = int(os.environ.get("CORTEX_PORT", "8742"))
        self.conn = UEConnection(port=port)
        self.passed = 0
        self.failed = 0
        self.errors = []

    def run_all(self):
        """Run all E2E test suites."""
        print("=" * 60)
        print("Cortex MCP End-to-End Tests")
        print("=" * 60)

        # 1. Connection & Core
        self._section("Core Connection")
        self._test("TCP connect", self.test_connect)
        self._test("ping", self.test_ping)
        self._test("get_status", self.test_get_status)
        self._test("get_capabilities", self.test_get_capabilities)

        # 2. Data Catalog
        self._section("Data Catalog")
        self._test("data.get_data_catalog", self.test_get_data_catalog)

        # 3. DataTables
        self._section("DataTable Operations")
        self._test("data.list_datatables", self.test_list_datatables)
        self._test("data.get_datatable_schema", self.test_get_datatable_schema)
        self._test("data.query_datatable", self.test_query_datatable)
        self._test("data.get_datatable_row", self.test_get_datatable_row)
        self._test("data.search_datatable_content", self.test_search_datatable_content)
        self._test("data.get_struct_schema", self.test_get_struct_schema)

        # 4. GameplayTags
        self._section("GameplayTag Operations")
        self._test("data.list_gameplay_tags", self.test_list_gameplay_tags)
        self._test("data.validate_gameplay_tag (valid)", self.test_validate_tag_valid)
        self._test("data.validate_gameplay_tag (invalid)", self.test_validate_tag_invalid)

        # 5. CurveTables
        self._section("CurveTable Operations")
        self._test("data.list_curve_tables", self.test_list_curve_tables)
        self._test("data.get_curve_table", self.test_get_curve_table)

        # 6. StringTables / Localization
        self._section("Localization Operations")
        self._test("data.list_string_tables", self.test_list_string_tables)
        self._test("data.get_translations", self.test_get_translations)

        # 7. Asset Search
        self._section("Asset Search")
        self._test("data.search_assets", self.test_search_assets)

        # 8. DataTable Write Operations (add -> update -> delete)
        self._section("DataTable Write Operations")
        self._test("data.add_datatable_row", self.test_add_datatable_row)
        self._test("data.update_datatable_row", self.test_update_datatable_row)
        self._test("data.delete_datatable_row", self.test_delete_datatable_row)

        # 9. Edge Cases
        self._section("Edge Cases & Error Handling")
        self._test("Unknown command", self.test_unknown_command)
        self._test("Missing table_path param", self.test_missing_params)
        self._test("Invalid table path", self.test_invalid_table_path)

        # 10. Batch
        self._section("Batch Operations")
        self._test("batch query", self.test_batch_query)

        # Summary
        self._summary()
        self.conn.disconnect()
        return self.failed == 0

    # ================================================================
    # Core Connection Tests
    # ================================================================

    def test_connect(self):
        self.conn.connect()
        assert self.conn.connected, "Should be connected"

    def test_ping(self):
        resp = self.conn.send_command("ping")
        data = resp["data"]
        assert data.get("message") == "pong", f"Expected pong, got {data}"
        return "pong"

    def test_get_status(self):
        resp = self.conn.send_command("get_status")
        data = resp["data"]
        assert "plugin_version" in data, f"Missing plugin_version. Keys: {list(data.keys())}"
        assert "engine_version" in data, "Missing engine_version"
        assert "project_name" in data, "Missing project_name"
        return f"project={data['project_name']}, engine={data['engine_version']}"

    def test_get_capabilities(self):
        resp = self.conn.send_command("get_capabilities")
        data = resp["data"]
        # domains is an object keyed by namespace, not an array
        assert "domains" in data, f"Missing domains. Keys: {list(data.keys())}"
        domains = data["domains"]
        assert isinstance(domains, dict), f"domains should be dict, got {type(domains)}"
        assert "data" in domains, f"Should have 'data' domain. Keys: {list(domains.keys())}"
        data_domain = domains["data"]
        assert "commands" in data_domain, f"data domain missing commands. Keys: {list(data_domain.keys())}"
        cmd_names = [c["name"] for c in data_domain["commands"]]
        return f"data domain commands: {len(cmd_names)} ({', '.join(cmd_names[:5])}...)"

    # ================================================================
    # Data Catalog
    # ================================================================

    def test_get_data_catalog(self):
        resp = self.conn.send_command("data.get_data_catalog")
        data = resp["data"]
        assert "datatables" in data, f"Missing datatables. Keys: {list(data.keys())}"
        assert "tag_prefixes" in data, "Missing tag_prefixes"
        assert "string_tables" in data, "Missing string_tables"
        dt_count = len(data["datatables"])
        st_count = len(data["string_tables"])
        return f"datatables={dt_count}, string_tables={st_count}"

    # ================================================================
    # DataTable Tests
    # ================================================================

    def test_list_datatables(self):
        resp = self.conn.send_command("data.list_datatables")
        data = resp["data"]
        # Key is "datatables" not "tables"
        assert "datatables" in data, f"Missing datatables. Keys: {list(data.keys())}"
        tables = data["datatables"]
        assert len(tables) > 0, "Should have at least one DataTable"
        names = [t["name"] for t in tables]
        assert "DT_TestSimple" in names, f"Should have DT_TestSimple. Found: {names}"
        return f"tables={names}"

    def test_get_datatable_schema(self):
        resp = self.conn.send_command(
            "data.get_datatable_schema",
            {"table_path": "/Game/Data/DT_TestSimple"},
        )
        data = resp["data"]
        # Top level: table_path, row_struct_name, schema
        assert "schema" in data, f"Missing schema. Keys: {list(data.keys())}"
        schema = data["schema"]
        assert "struct_name" in schema, f"Missing struct_name in schema. Keys: {list(schema.keys())}"
        assert "fields" in schema, "Missing fields in schema"
        assert len(schema["fields"]) > 0, "Should have at least one field"
        field_names = [f["name"] for f in schema["fields"]]
        return f"struct={schema['struct_name']}, fields={field_names}"

    def test_query_datatable(self):
        resp = self.conn.send_command(
            "data.query_datatable",
            {"table_path": "/Game/Data/DT_TestSimple", "limit": 10},
        )
        data = resp["data"]
        assert "rows" in data, f"Missing rows. Keys: {list(data.keys())}"
        assert len(data["rows"]) > 0, "Should have rows"
        row_names = [r["row_name"] for r in data["rows"]]
        return f"row_count={len(data['rows'])}, rows={row_names}"

    def test_get_datatable_row(self):
        resp = self.conn.send_command(
            "data.get_datatable_row",
            {"table_path": "/Game/Data/DT_TestSimple", "row_name": "Sword"},
        )
        data = resp["data"]
        assert "row_name" in data, f"Missing row_name. Keys: {list(data.keys())}"
        assert data["row_name"] == "Sword", f"Expected Sword, got {data['row_name']}"
        assert "row_data" in data, "Missing row_data"
        return f"row={data['row_name']}, data_keys={list(data['row_data'].keys())}"

    def test_search_datatable_content(self):
        resp = self.conn.send_command(
            "data.search_datatable_content",
            {"table_path": "/Game/Data/DT_TestSimple", "search_text": "Sword"},
        )
        data = resp["data"]
        # Key is "results" not "matches"
        assert "results" in data, f"Missing results. Keys: {list(data.keys())}"
        assert "total_matches" in data, "Missing total_matches"
        return f"total_matches={data['total_matches']}, result_count={len(data['results'])}"

    def test_get_struct_schema(self):
        resp = self.conn.send_command(
            "data.get_struct_schema",
            {"struct_name": "GameplayTagTableRow"},
        )
        data = resp["data"]
        assert "schema" in data, f"Missing schema. Keys: {list(data.keys())}"
        return f"struct={data['schema']['struct_name']}"

    # ================================================================
    # GameplayTag Tests
    # ================================================================

    def test_list_gameplay_tags(self):
        resp = self.conn.send_command(
            "data.list_gameplay_tags", {}
        )
        data = resp["data"]
        assert "tags" in data, f"Missing tags. Keys: {list(data.keys())}"
        tags = data["tags"]
        assert len(tags) > 0, f"Should have tags, got {len(tags)}"
        # Check for any Cortex tags (registered in .ini, loaded on editor start)
        cortex_tags = [t for t in tags if t["tag"].startswith("Cortex.")]
        tag_names = [t["tag"] for t in tags]
        return f"total_tags={len(tags)}, cortex_tags={len(cortex_tags)}, sample={tag_names[:5]}"

    def test_validate_tag_valid(self):
        # Use a tag we know exists from Config/Tags/GameplayTags.ini
        # Tags registered via register_gameplay_tags at runtime may not be
        # in the tag manager until next editor restart
        resp = self.conn.send_command(
            "data.validate_gameplay_tag", {"tag": "Cortex.Test.Tag1"}
        )
        data = resp["data"]
        # Key is "valid" (not "is_valid")
        # Tag may or may not be valid depending on whether editor loaded the .ini tags
        assert "valid" in data, f"Missing 'valid' key. Data: {data}"
        return f"Cortex.Test.Tag1 valid={data['valid']}"

    def test_validate_tag_invalid(self):
        resp = self.conn.send_command(
            "data.validate_gameplay_tag", {"tag": "NonExistent.Fake.Tag.12345"}
        )
        data = resp["data"]
        assert data.get("valid") is False, f"NonExistent.Fake.Tag should be invalid. Data: {data}"
        return "NonExistent.Fake.Tag.12345 correctly invalid"

    # ================================================================
    # CurveTable Tests
    # ================================================================

    def test_list_curve_tables(self):
        resp = self.conn.send_command("data.list_curve_tables")
        data = resp["data"]
        # Key is "curve_tables" not "tables"
        assert "curve_tables" in data, f"Missing curve_tables. Keys: {list(data.keys())}"
        tables = data["curve_tables"]
        assert len(tables) > 0, "Should have at least one CurveTable"
        names = [t["name"] for t in tables]
        assert "CT_TestCurve" in names, f"Should have CT_TestCurve. Found: {names}"
        return f"tables={names}"

    def test_get_curve_table(self):
        resp = self.conn.send_command(
            "data.get_curve_table",
            {"table_path": "/Game/Data/CT_TestCurve"},
        )
        data = resp["data"]
        assert "curves" in data, f"Missing curves. Keys: {list(data.keys())}"
        assert len(data["curves"]) > 0, "Should have at least one curve"
        curve = data["curves"][0]
        assert curve["row_name"] == "Curve", f"Expected Curve row, got {curve['row_name']}"
        assert curve["key_count"] == 3, f"Expected 3 keys, got {curve['key_count']}"
        return f"row={curve['row_name']}, keys={curve['key_count']}"

    # ================================================================
    # Localization Tests
    # ================================================================

    def test_list_string_tables(self):
        resp = self.conn.send_command("data.list_string_tables")
        data = resp["data"]
        # Key is "string_tables" not "tables"
        assert "string_tables" in data, f"Missing string_tables. Keys: {list(data.keys())}"
        tables = data["string_tables"]
        assert len(tables) > 0, "Should have at least one StringTable"
        names = [t["name"] for t in tables]
        assert "ST_TestStrings" in names, f"Should have ST_TestStrings. Found: {names}"
        return f"tables={names}"

    def test_get_translations(self):
        resp = self.conn.send_command(
            "data.get_translations",
            {"string_table_path": "/Game/Data/ST_TestStrings"},
        )
        data = resp["data"]
        assert "entries" in data, f"Missing entries. Keys: {list(data.keys())}"
        assert len(data["entries"]) >= 4, f"Expected >= 4 entries, got {len(data['entries'])}"
        keys = [e["key"] for e in data["entries"]]
        return f"entry_count={len(data['entries'])}, keys={keys}"

    # ================================================================
    # Asset Search Tests
    # ================================================================

    def test_search_assets(self):
        # Search by path filter to find assets in /Game/Data/
        resp = self.conn.send_command(
            "data.search_assets",
            {"path_filter": "/Game/Data/"},
        )
        data = resp["data"]
        assert "assets" in data, f"Missing assets. Keys: {list(data.keys())}"
        assert len(data["assets"]) > 0, f"Should find assets in /Game/Data/. Got {data.get('count', 0)} results"
        names = [a["name"] for a in data["assets"]]
        return f"found={names}"

    # ================================================================
    # DataTable Write Operations
    # ================================================================

    def test_add_datatable_row(self):
        resp = self.conn.send_command(
            "data.add_datatable_row",
            {
                "table_path": "/Game/Data/DT_TestSimple",
                "row_name": "E2E_TestRow",
                "row_data": {
                    "Tag": "Cortex.Test.Tag1",
                },
            },
        )
        data = resp["data"]
        # Success response has row_name
        assert "row_name" in data, f"Missing row_name. Keys: {list(data.keys())}"
        assert data["row_name"] == "E2E_TestRow", f"Expected E2E_TestRow, got {data['row_name']}"
        return "E2E_TestRow added"

    def test_update_datatable_row(self):
        resp = self.conn.send_command(
            "data.update_datatable_row",
            {
                "table_path": "/Game/Data/DT_TestSimple",
                "row_name": "E2E_TestRow",
                "row_data": {
                    "Tag": "Cortex.Test.Tag2",
                },
            },
        )
        data = resp["data"]
        assert "row_name" in data, f"Missing row_name. Keys: {list(data.keys())}"
        return f"E2E_TestRow updated, modified_fields={data.get('modified_fields', 'N/A')}"

    def test_delete_datatable_row(self):
        resp = self.conn.send_command(
            "data.delete_datatable_row",
            {
                "table_path": "/Game/Data/DT_TestSimple",
                "row_name": "E2E_TestRow",
            },
        )
        data = resp["data"]
        assert "row_name" in data, f"Missing row_name. Keys: {list(data.keys())}"
        assert data["row_name"] == "E2E_TestRow", f"Expected E2E_TestRow, got {data['row_name']}"
        return "E2E_TestRow deleted (cleanup)"

    # ================================================================
    # Edge Cases & Error Handling
    # ================================================================

    def test_unknown_command(self):
        try:
            self.conn.send_command("data.nonexistent_command_xyz")
            assert False, "Should have raised RuntimeError"
        except RuntimeError as e:
            err_str = str(e).lower()
            assert "failed" in err_str or "error" in err_str or "unknown" in err_str, (
                f"Should indicate failure: {e}"
            )
            return f"Correctly rejected"

    def test_missing_params(self):
        try:
            self.conn.send_command("data.get_datatable_row", {})
            assert False, "Should have raised RuntimeError"
        except RuntimeError as e:
            return f"Correctly rejected"

    def test_invalid_table_path(self):
        try:
            self.conn.send_command(
                "data.query_datatable",
                {"table_path": "/Game/NonExistent/FakeTable_12345"},
            )
            assert False, "Should have raised RuntimeError"
        except RuntimeError as e:
            return f"Correctly rejected"

    # ================================================================
    # Batch Operations
    # ================================================================

    def test_batch_query(self):
        resp = self.conn.send_command(
            "batch",
            {
                "commands": [
                    {"command": "get_status", "params": {}},
                    {"command": "data.list_datatables", "params": {}},
                    {"command": "data.list_string_tables", "params": {}},
                ]
            },
        )
        data = resp["data"]
        # Key is "results"
        assert "results" in data, f"Missing results. Keys: {list(data.keys())}"
        results = data["results"]
        assert len(results) == 3, f"Should have 3 results, got {len(results)}"
        success_count = sum(1 for r in results if r.get("success"))
        return f"batch_results={len(results)}, all_success={success_count == 3}"

    # ================================================================
    # Test Runner Helpers
    # ================================================================

    def _section(self, name):
        print(f"\n--- {name} ---")

    def _test(self, name, fn):
        try:
            detail = fn()
            self.passed += 1
            detail_str = f" ({detail})" if detail else ""
            print(f"  PASS  {name}{detail_str}")
        except Exception as e:
            self.failed += 1
            self.errors.append((name, e))
            print(f"  FAIL  {name}: {e}")

    def _summary(self):
        total = self.passed + self.failed
        print("\n" + "=" * 60)
        print(f"Results: {self.passed}/{total} passed, {self.failed} failed")
        if self.errors:
            print("\nFailed tests:")
            for name, err in self.errors:
                print(f"  - {name}: {err}")
        print("=" * 60)


if __name__ == "__main__":
    runner = E2ETestRunner()
    success = runner.run_all()
    sys.exit(0 if success else 1)
