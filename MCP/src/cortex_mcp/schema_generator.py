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
