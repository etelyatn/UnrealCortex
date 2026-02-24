"""E2E test for schema generation (requires running Unreal Editor)."""

import tempfile
from pathlib import Path

import pytest

from cortex_mcp.schema_generator import generate_schema, SCHEMA_VERSION


class TestSchemaE2E:
    """End-to-end schema generation against live Unreal Editor."""

    def test_generate_data_schema(self, tcp_connection):
        """Generate data.md from live editor and verify structure."""
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            result = generate_schema(
                connection=tcp_connection,
                schema_dir=schema_dir,
                domain="data",
                project_name="CortexSandbox",
            )

            assert "data" in result["generated"]
            assert len(result["errors"]) == 0

            # data.md should exist and contain real content
            data_md = schema_dir / "data.md"
            assert data_md.exists()
            content = data_md.read_text(encoding="utf-8")
            assert "## DataTables" in content
            assert "## Struct Schemas" in content
            assert "schema-meta" in content

            # _catalog.md should exist
            catalog_md = schema_dir / "_catalog.md"
            assert catalog_md.exists()
            catalog_content = catalog_md.read_text(encoding="utf-8")
            assert "## Schema Overview" in catalog_content
            assert "## Schema Index" in catalog_content

    def test_generate_all(self, tcp_connection):
        """Generate all domains and verify catalog has index."""
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            result = generate_schema(
                connection=tcp_connection,
                schema_dir=schema_dir,
                domain="all",
                project_name="CortexSandbox",
            )

            assert "_catalog" in result["generated"]
            catalog_content = (schema_dir / "_catalog.md").read_text(encoding="utf-8")

            # Catalog should have overview table
            assert "| data |" in catalog_content

    def test_schema_status_after_generate(self, tcp_connection):
        """schema_status should report generated files."""
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            generate_schema(
                connection=tcp_connection,
                schema_dir=schema_dir,
                domain="data",
                project_name="CortexSandbox",
            )

            # Read the generated data.md meta
            content = (schema_dir / "data.md").read_text(encoding="utf-8")
            assert f"schema_version: {SCHEMA_VERSION}" in content
