"""E2E test for schema generation (requires running Unreal Editor)."""

import tempfile
from pathlib import Path

import pytest

from cortex_mcp.schema_generator import generate_schema, SCHEMA_VERSION


@pytest.mark.e2e
class TestSchemaE2E:
    """End-to-end schema generation against live Unreal Editor."""

    def test_generate_data_schema(self, tcp_connection):
        """Generate data/ files from live editor and verify structure."""
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            result = generate_schema(
                connection=tcp_connection,
                schema_dir=schema_dir,
                domain="data",
                project_name="CortexSandbox",
            )

            assert "data_index" in result["generated"]
            assert "data_structs" in result["generated"]
            assert "data_formats" in result["generated"]
            assert len(result["errors"]) == 0

            # data/_index.md should exist with the index header
            index_md = schema_dir / "data" / "_index.md"
            assert index_md.exists()
            content = index_md.read_text(encoding="utf-8")
            assert "schema-meta" in content
            assert "# Data Domain Index" in content

            # data/structs.md should exist with struct header
            structs_md = schema_dir / "data" / "structs.md"
            assert structs_md.exists()
            structs_content = structs_md.read_text(encoding="utf-8")
            assert "# Data Struct Schemas" in structs_content

            # Old v1 monolithic file must NOT exist
            assert not (schema_dir / "data.md").exists()

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

            # Catalog should reference data subdirectory
            assert "data/_index.md" in catalog_content

    def test_schema_status_after_generate(self, tcp_connection):
        """Schema files should carry correct version after generation."""
        with tempfile.TemporaryDirectory() as tmpdir:
            schema_dir = Path(tmpdir) / ".cortex" / "schema"
            generate_schema(
                connection=tcp_connection,
                schema_dir=schema_dir,
                domain="data",
                project_name="CortexSandbox",
            )

            # Read the generated data/_index.md meta
            content = (schema_dir / "data" / "_index.md").read_text(encoding="utf-8")
            assert f"schema_version: {SCHEMA_VERSION}" in content
