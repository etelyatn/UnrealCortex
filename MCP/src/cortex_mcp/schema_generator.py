"""Generate LLM-readable schema files in .cortex/schema/."""

import datetime
import json
import logging
import os
import pathlib
import tempfile

logger = logging.getLogger(__name__)

SCHEMA_VERSION = 1


def _get_caller_path() -> pathlib.Path:
    """Get the path of this file (used for .uproject walk-up)."""
    return pathlib.Path(__file__).resolve().parent


def find_project_root() -> pathlib.Path:
    """Find the Unreal project root directory.

    Uses CORTEX_PROJECT_DIR env var if set, otherwise walks up to find .uproject.
    """
    project_dir = os.environ.get("CORTEX_PROJECT_DIR")
    if project_dir:
        return pathlib.Path(project_dir)

    current = _get_caller_path()
    for _ in range(20):
        if list(current.glob("*.uproject")):
            return current
        parent = current.parent
        if parent == current:
            break
        current = parent

    raise FileNotFoundError("Cannot find .uproject file. Set CORTEX_PROJECT_DIR env var.")


def get_schema_dir() -> pathlib.Path:
    """Get the .cortex/schema/ directory path."""
    return find_project_root() / ".cortex" / "schema"


def atomic_write(path: pathlib.Path, content: str) -> None:
    """Write content to a file atomically via temp file + rename."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_fd, tmp_path = tempfile.mkstemp(
        suffix=".tmp", dir=str(path.parent)
    )
    try:
        with os.fdopen(tmp_fd, "w", encoding="utf-8") as f:
            f.write(content)
        os.replace(tmp_path, path)
    except Exception:
        # Clean up temp file on failure
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise


def _yaml_field(field: dict, indent: int = 2) -> str:
    """Render a single schema field as YAML."""
    prefix = " " * indent
    lines = [f"{prefix}- name: {field['name']}"]
    lines.append(f"{prefix}  type: {field.get('type', field.get('cpp_type', 'unknown'))}")
    if field.get("default_value"):
        lines.append(f"{prefix}  default: {field['default_value']}")
    if field.get("enum_values"):
        vals = ", ".join(field["enum_values"])
        lines.append(f"{prefix}  enum: [{vals}]")
    if field.get("element_type"):
        elem = field["element_type"]
        if isinstance(elem, dict):
            lines.append(f"{prefix}  element_type: {elem.get('type', elem.get('cpp_type', ''))}")
    if field.get("fields"):
        lines.append(f"{prefix}  nested_fields:")
        for sub in field["fields"]:
            lines.extend(_yaml_field(sub, indent + 4).split("\n"))
    return "\n".join(lines)


def _render_meta(domain: str) -> str:
    """Render the schema-meta HTML comment block."""
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return (
        f"<!-- schema-meta\n"
        f"schema_version: {SCHEMA_VERSION}\n"
        f"generated: {now}\n"
        f"domain: {domain}\n"
        f"-->"
    )


def render_data_schema(
    catalog: dict,
    schemas: dict[str, dict],
    example_rows: dict[str, list],
    curve_tables: list[dict],
    enum_values: dict[str, list[str]],
) -> str:
    """Render the data domain schema as Markdown+YAML.

    Args:
        catalog: Response from data.get_data_catalog (datatables, tag_prefixes, etc.)
        schemas: Dict of struct_name -> schema response from data.get_datatable_schema
        example_rows: Dict of table_name -> list of {row_name, row_data} (1-2 per table)
        curve_tables: List of curve table entries from data.list_curve_tables
        enum_values: Dict of enum_name -> list of string values
    """
    lines = ["# Data Domain Schema", "", _render_meta("data"), ""]

    # DataTables summary table
    tables = catalog.get("datatables", [])
    if tables:
        lines.append("## DataTables")
        lines.append("")
        lines.append("| Table | Struct | Rows | Path |")
        lines.append("|-------|--------|------|------|")
        for t in tables:
            lines.append(
                f"| {t['name']} | {t['row_struct']} | {t['row_count']} | {t['path'].split('.')[0]} |"
            )
        lines.append("")

    # Struct schemas
    if schemas:
        lines.append("## Struct Schemas")
        lines.append("")
        for struct_name, schema_data in schemas.items():
            lines.append(f"### {struct_name}")
            lines.append("```yaml")
            parent = schema_data.get("parent", "FTableRowBase")
            lines.append(f"parent: {parent}")
            fields = schema_data.get("schema", [])
            if fields:
                lines.append("fields:")
                for field in fields:
                    lines.append(_yaml_field(field))
            lines.append("```")
            lines.append("")

    # Example rows
    if example_rows:
        lines.append("## Example Rows")
        lines.append("")
        for table_name, rows in example_rows.items():
            total = next(
                (t["row_count"] for t in tables if t["name"] == table_name), len(rows)
            )
            lines.append(f"### {table_name} ({len(rows)} of {total})")
            lines.append("```yaml")
            for row in rows:
                lines.append(f"- RowName: {row['row_name']}")
                for k, v in row.get("row_data", {}).items():
                    if isinstance(v, list):
                        lines.append(f"  {k}: {json.dumps(v)}")
                    elif isinstance(v, str):
                        lines.append(f'  {k}: "{v}"')
                    else:
                        lines.append(f"  {k}: {v}")
                lines.append("")
            lines.append("```")
            lines.append("")

    # GameplayTag prefixes
    tag_prefixes = catalog.get("tag_prefixes", [])
    if tag_prefixes:
        lines.append("## GameplayTag Prefixes")
        lines.append("")
        lines.append("```yaml")
        lines.append("prefixes:")
        for tp in tag_prefixes:
            lines.append(f"  - prefix: {tp['prefix']}")
            lines.append(f"    count: {tp['count']}")
        lines.append("```")
        lines.append("")

    # DataAsset classes
    asset_classes = catalog.get("data_asset_classes", [])
    if asset_classes:
        lines.append("## DataAsset Classes")
        lines.append("")
        lines.append("```yaml")
        lines.append("classes:")
        for ac in asset_classes:
            lines.append(f"  - name: {ac['class_name']}")
            lines.append(f"    count: {ac['count']}")
            if ac.get("example_path"):
                lines.append(f"    example: {ac['example_path']}")
        lines.append("```")
        lines.append("")

    # CurveTable summaries
    if curve_tables:
        lines.append("## CurveTables")
        lines.append("")
        lines.append("| Name | Rows | Type | Path |")
        lines.append("|------|------|------|------|")
        for ct in curve_tables:
            lines.append(
                f"| {ct['name']} | {ct.get('row_count', '?')} "
                f"| {ct.get('curve_type', '?')} | {ct['path'].split('.')[0]} |"
            )
        lines.append("")

    # StringTable summaries
    string_tables = catalog.get("string_tables", [])
    if string_tables:
        lines.append("## StringTables")
        lines.append("")
        lines.append("| Name | Entries | Path |")
        lines.append("|------|---------|------|")
        for st in string_tables:
            lines.append(
                f"| {st['name']} | {st.get('entry_count', '?')} | {st['path'].split('.')[0]} |"
            )
        lines.append("")

    # Enum values
    if enum_values:
        lines.append("## Enum Values")
        lines.append("")
        lines.append("```yaml")
        lines.append("enums:")
        for enum_name, values in enum_values.items():
            vals = ", ".join(values)
            lines.append(f"  - name: {enum_name}")
            lines.append(f"    values: [{vals}]")
        lines.append("```")
        lines.append("")

    return "\n".join(lines)


def render_catalog(
    project_name: str,
    data_summary: dict | None = None,
    blueprint_summary: dict | None = None,
) -> str:
    """Render _catalog.md with overview and type-grouped index.

    Args:
        project_name: Name of the Unreal project.
        data_summary: Dict with structs, tables, tag_prefixes, data_assets for data domain.
        blueprint_summary: Dict with classes for blueprint domain (future).
    """
    lines = ["# Cortex Schema Catalog", ""]
    meta = _render_meta("catalog").replace("domain: catalog", f"project: {project_name}")
    lines.append(meta)
    lines.append("")

    # How to Use
    lines.append("## How to Use")
    lines.append("- Read THIS file first for project overview and index")
    lines.append("- Read domain files only when working in that domain")
    lines.append("- If a domain file is missing, use live MCP tools instead")
    lines.append("- If generated timestamp is older than 24h, suggest /cortex-schema-refresh")
    lines.append("")

    # Schema Overview
    lines.append("## Schema Overview")
    lines.append("")
    lines.append("| Domain | File | Structs | Tables | Tags | Assets | Blueprints |")
    lines.append("|--------|------|---------|--------|------|--------|------------|")

    data_s = data_summary or {}
    data_structs = len(data_s.get("structs", []))
    data_tables = len(data_s.get("tables", []))
    data_tags = len(data_s.get("tag_prefixes", []))
    data_assets = len(data_s.get("data_assets", []))
    if data_structs or data_tables or data_tags or data_assets:
        lines.append(
            f"| data | data.md | {data_structs} | {data_tables} "
            f"| {data_tags} prefixes | {data_assets} | — |"
        )

    bp_s = blueprint_summary or {}
    bp_classes = len(bp_s.get("classes", []))
    if bp_classes:
        lines.append(f"| blueprints | blueprints.md | — | — | — | — | {bp_classes} |")

    lines.append("")

    # Schema Index
    lines.append("## Schema Index")
    lines.append("")

    # Data domain index
    if data_s:
        lines.append("### data.md")
        lines.append("")

        structs = data_s.get("structs", [])
        if structs:
            lines.append("#### Structs")
            lines.append("| Name | Used By |")
            lines.append("|------|---------|")
            for s in structs:
                lines.append(f"| {s['name']} | {s['used_by']} |")
            lines.append("")

        tables = data_s.get("tables", [])
        if tables:
            lines.append("#### DataTables")
            lines.append("| Name | Row Struct | Rows |")
            lines.append("|------|------------|------|")
            for t in tables:
                lines.append(f"| {t['name']} | {t['row_struct']} | {t['rows']} |")
            lines.append("")

        tags = data_s.get("tag_prefixes", [])
        if tags:
            lines.append("#### GameplayTag Prefixes")
            lines.append("| Prefix | Count |")
            lines.append("|--------|-------|")
            for tp in tags:
                lines.append(f"| {tp['prefix']} | {tp['count']} |")
            lines.append("")

        assets = data_s.get("data_assets", [])
        if assets:
            lines.append("#### DataAssets")
            lines.append("| Class | Instances |")
            lines.append("|-------|-----------|")
            for a in assets:
                lines.append(f"| {a['class']} | {a['instances']} |")
            lines.append("")

    # Blueprint domain index (future)
    if bp_s:
        lines.append("### blueprints.md")
        lines.append("")
        bp_classes_list = bp_s.get("classes", [])
        if bp_classes_list:
            lines.append("#### Blueprint Classes")
            lines.append("| Name | Parent |")
            lines.append("|------|--------|")
            for c in bp_classes_list:
                lines.append(f"| {c['name']} | {c['parent']} |")
            lines.append("")

    return "\n".join(lines)


def collect_data_domain(connection) -> dict:
    """Collect all data domain information from a live UE editor.

    Calls existing TCP commands and assembles the raw data.

    Args:
        connection: UEConnection instance (connected to running editor).

    Returns:
        Dict with catalog, schemas, example_rows, curve_tables, enum_values, summary.
    """
    # 1. Get catalog
    catalog_resp = connection.send_command("data.get_data_catalog", {})
    catalog = catalog_resp.get("data", {})

    # 2. Get schemas for each unique row struct
    schemas = {}
    struct_to_tables = {}  # struct_name -> [table_name, ...]
    for table in catalog.get("datatables", []):
        struct_name = table["row_struct"]
        if struct_name not in struct_to_tables:
            struct_to_tables[struct_name] = []
        struct_to_tables[struct_name].append(table["name"])

        if struct_name not in schemas:
            try:
                resp = connection.send_command(
                    "data.get_datatable_schema",
                    {"table_path": table["path"], "include_inherited": True},
                )
                schema_data = resp.get("data", {})
                schemas[struct_name] = {
                    "struct_name": struct_name,
                    "parent": schema_data.get("parent_struct", "FTableRowBase"),
                    "schema": schema_data.get("schema", []),
                }
            except (RuntimeError, ConnectionError) as e:
                logger.warning("Failed to get schema for %s: %s", struct_name, e)

    # 3. Get 1-2 example rows per table
    example_rows = {}
    for table in catalog.get("datatables", []):
        try:
            resp = connection.send_command(
                "data.query_datatable",
                {"table_path": table["path"], "limit": 2, "offset": 0},
            )
            rows = resp.get("data", {}).get("rows", [])
            if rows:
                example_rows[table["name"]] = rows[:2]
        except (RuntimeError, ConnectionError) as e:
            logger.warning("Failed to get example rows for %s: %s", table["name"], e)

    # 4. Get curve tables
    curve_tables = []
    try:
        resp = connection.send_command("data.list_curve_tables", {})
        curve_tables = resp.get("data", {}).get("curve_tables", [])
    except (RuntimeError, ConnectionError) as e:
        logger.warning("Failed to list curve tables: %s", e)

    # 5. Extract enum values from schemas
    enum_values = {}
    for schema_data in schemas.values():
        for field in schema_data.get("schema", []):
            if field.get("enum_values"):
                enum_name = field.get("cpp_type", field.get("type", "Unknown"))
                if enum_name not in enum_values:
                    enum_values[enum_name] = field["enum_values"]

    # 6. Build summary for catalog index
    summary = {
        "structs": [
            {"name": name, "used_by": ", ".join(struct_to_tables.get(name, []))}
            for name in schemas
        ],
        "tables": [
            {"name": t["name"], "row_struct": t["row_struct"], "rows": t["row_count"]}
            for t in catalog.get("datatables", [])
        ],
        "tag_prefixes": [
            {"prefix": f"{tp['prefix']}.*", "count": tp["count"]}
            for tp in catalog.get("tag_prefixes", [])
        ],
        "data_assets": [
            {"class": ac["class_name"], "instances": ac["count"]}
            for ac in catalog.get("data_asset_classes", [])
        ],
    }

    return {
        "catalog": catalog,
        "schemas": schemas,
        "example_rows": example_rows,
        "curve_tables": curve_tables,
        "enum_values": enum_values,
        "summary": summary,
    }
