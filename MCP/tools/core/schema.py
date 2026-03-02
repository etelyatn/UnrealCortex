"""MCP tools for project schema generation and status."""

import json
import logging
import time

from cortex_mcp.schema_generator import (
    find_project_root,
    generate_schema,
    get_schema_dir,
    read_meta_from_file,
    SCHEMA_VERSION,
)
from cortex_mcp.tcp_client import UEConnection

logger = logging.getLogger(__name__)


def register_schema_tools(mcp, connection: UEConnection):
    """Register schema generation and status tools."""

    @mcp.tool()
    def generate_project_schema(domain: str = "all") -> str:
        """Generate LLM-readable schema files in .cortex/schema/.

        Creates Markdown+YAML files with project data structure:
        DataTable catalog, struct schemas, GameplayTag prefixes,
        DataAsset inventories, and example rows.

        Requires a running Unreal Editor with UnrealCortex plugin.

        Args:
            domain: Which domain to generate. "all" or "data".
                    Default "all" generates everything.

        Returns:
            JSON with generated file paths and any errors.
        """
        try:
            schema_dir = get_schema_dir()
            project_name = find_project_root().stem
            start = time.time()

            result = generate_schema(
                connection=connection,
                schema_dir=schema_dir,
                domain=domain,
                project_name=project_name,
            )

            result["schema_dir"] = str(schema_dir)
            result["elapsed_seconds"] = round(time.time() - start, 2)
            return json.dumps(result, indent=2)
        except ConnectionError as e:
            return json.dumps({"error": str(e), "suggestion": "Is the Unreal Editor running?"})
        except FileNotFoundError as e:
            return json.dumps({"error": str(e)})

    @mcp.tool()
    def schema_status() -> str:
        """Check if .cortex/schema/ exists and is fresh.

        Returns per-domain status with generation timestamps.
        No editor connection required — reads files from disk.

        Returns:
            JSON with per-domain status, schema version, and freshness.
        """
        try:
            schema_dir = get_schema_dir()
        except FileNotFoundError:
            return json.dumps({"error": "Cannot find project root"})

        if not schema_dir.exists():
            return json.dumps({
                "exists": False,
                "suggestion": "Run generate_project_schema to create schema files.",
            })

        domains = {}

        # Scan root-level domain files (v1 layout)
        for md_file in schema_dir.glob("*.md"):
            if md_file.name.startswith("_") or md_file.name == "README.md":
                continue
            meta = read_meta_from_file(md_file)
            domain_name = md_file.stem
            if meta:
                generated = meta.get("generated", "unknown")
                version = int(meta.get("schema_version", "0"))
                domains[domain_name] = {
                    "file": md_file.name,
                    "generated": generated,
                    "schema_version": version,
                    "version_current": version == SCHEMA_VERSION,
                }
            else:
                domains[domain_name] = {
                    "file": md_file.name,
                    "generated": "unknown",
                    "error": "No meta block found",
                }

        # Scan subdirectories (v2 layout)
        for subdir in schema_dir.iterdir():
            if not subdir.is_dir() or subdir.name.startswith("_"):
                continue
            domain_name = subdir.name
            files = {}
            oldest_generated = None
            for md_file in sorted(subdir.glob("*.md")):
                meta = read_meta_from_file(md_file)
                if meta:
                    files[md_file.name] = meta.get("generated", "unknown")
                    gen = meta.get("generated")
                    if oldest_generated is None or (gen and gen < oldest_generated):
                        oldest_generated = gen
            if files:
                first_file = next(iter(sorted(subdir.glob("*.md"))))
                first_meta = read_meta_from_file(first_file)
                version = int(first_meta.get("schema_version", "0")) if first_meta else 0
                domains[domain_name] = {
                    "files": files,
                    "generated": oldest_generated or "unknown",
                    "schema_version": version,
                    "version_current": version == SCHEMA_VERSION,
                }

        catalog_meta = read_meta_from_file(schema_dir / "_catalog.md")

        return json.dumps({
            "exists": True,
            "schema_dir": str(schema_dir),
            "catalog": {
                "generated": catalog_meta.get("generated", "unknown") if catalog_meta else "missing",
            },
            "domains": domains,
            "current_schema_version": SCHEMA_VERSION,
        }, indent=2)
