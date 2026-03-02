"""Generate LLM-readable schema files in .cortex/schema/."""

import datetime
import json
import logging
import os
import pathlib
import tempfile
from typing import Any

logger = logging.getLogger(__name__)

SCHEMA_VERSION = 2


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


def _yaml_field(field: dict, indent: int = 2) -> list[str]:
    """Render a single schema field as YAML lines."""
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
            lines.extend(_yaml_field(sub, indent + 4))
    return lines


def _render_meta(domain: str, **extra) -> str:
    """Render the schema-meta HTML comment block.

    Args:
        domain: The domain name (e.g., "data", "catalog").
        **extra: Additional key-value pairs to include in the meta block.
    """
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        "<!-- schema-meta",
        f"schema_version: {SCHEMA_VERSION}",
        f"generated: {now}",
        f"domain: {domain}",
    ]
    for key, value in extra.items():
        lines.append(f"{key}: {value}")
    lines.append("-->")
    return "\n".join(lines)


def read_meta_from_file(path: pathlib.Path) -> dict | None:
    """Parse schema-meta comment block from a .md file.

    Companion to _render_meta — keeps read/write pair together.
    """
    if not path.exists():
        return None
    try:
        content = path.read_text(encoding="utf-8")
        start = content.find("<!-- schema-meta")
        end = content.find("-->", start)
        if start == -1 or end == -1:
            return None
        meta_text = content[start + len("<!-- schema-meta"):end].strip()
        meta = {}
        for line in meta_text.split("\n"):
            line = line.strip()
            if ":" in line:
                key, _, value = line.partition(":")
                meta[key.strip()] = value.strip()
        return meta
    except OSError:
        return None


def render_catalog(
    project_name: str,
    data_summary: dict | None = None,
    blueprint_summary: dict | None = None,
    engine_version: str = "",
    plugin_version: str = "",
) -> str:
    """Render _catalog.md with overview and type-grouped index.

    Args:
        project_name: Name of the Unreal project.
        data_summary: Dict with structs, tables, tag_prefixes, data_assets for data domain.
        blueprint_summary: Dict with classes for blueprint domain (future).
        engine_version: Unreal Engine version (e.g., "5.6").
        plugin_version: UnrealCortex plugin version (e.g., "1.0.0").
    """
    extra = {"project": project_name}
    if engine_version:
        extra["engine"] = engine_version
    if plugin_version:
        extra["plugin"] = plugin_version
    lines = ["# Cortex Schema Catalog", ""]
    lines.append(_render_meta("catalog", **extra))
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
            f"| data | data/_index.md, data/structs.md, data/formats.md | {data_structs} | {data_tables} "
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
        lines.append("### data/")
        lines.append("")
        lines.append("| File | Purpose | When to read |")
        lines.append("|------|---------|-------------|")
        lines.append("| data/_index.md | Table listing, tags, assets | Working with data domain |")
        lines.append("| data/structs.md | Struct field definitions | Need field types/names |")
        lines.append("| data/formats.md | Format examples (1 per struct) | Need serialization format |")
        lines.append("")

        structs = data_s.get("structs", [])
        if structs:
            lines.append(f"**Structs:** {', '.join(s['name'] for s in structs)}")
            lines.append("")

        tables = data_s.get("tables", [])
        if tables:
            lines.append(f"**Tables:** {len(tables)} total")
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


def _flatten_hierarchy(node: dict, rows: list[dict], depth: int = 0) -> None:
    """Flatten hierarchy tree into rows with depth metadata."""
    if not isinstance(node, dict):
        return

    rows.append(
        {
            "name": node.get("name", ""),
            "type": node.get("type", ""),
            "asset_path": node.get("asset_path", ""),
            "depth": depth,
            "parent": node.get("parent", ""),
        }
    )
    for child in node.get("children", []):
        if isinstance(child, dict):
            _flatten_hierarchy(child, rows, depth + 1)


def collect_blueprint_domain(connection) -> dict:
    """Collect Blueprint class hierarchy from reflect.class_hierarchy."""
    params = {
        "root": "AActor",
        "depth": 10,
        "max_results": 5000,
        "include_engine": False,
        "include_blueprint": True,
    }
    response = connection.send_command_cached(
        "reflect.class_hierarchy",
        params=params,
        ttl=3600,
    )
    data = _decode_data(response)
    rows: list[dict] = []
    _flatten_hierarchy(data, rows)
    classes = [
        {
            "name": row.get("name", ""),
            "parent": row.get("parent", ""),
            "type": row.get("type", ""),
            "asset_path": row.get("asset_path", ""),
            "depth": row.get("depth", 0),
        }
        for row in rows
    ]
    return {
        "hierarchy": data,
        "classes": classes,
        "blueprint_count": data.get("blueprint_count", 0),
        "cpp_count": data.get("cpp_count", 0),
        "project_cpp_count": data.get("project_cpp_count", 0),
        "engine_cpp_count": data.get("engine_cpp_count", 0),
        "project_blueprint_count": data.get("project_blueprint_count", 0),
    }


def render_blueprint_catalog(blueprint_data: dict) -> str:
    """Render blueprints.md with hierarchy and summary counts."""
    lines = ["# Blueprint Catalog", "", _render_meta("blueprints"), ""]

    bp_count = blueprint_data.get("blueprint_count", 0)
    cpp_count = blueprint_data.get("cpp_count", 0)
    project_cpp_count = blueprint_data.get("project_cpp_count", 0)
    engine_cpp_count = blueprint_data.get("engine_cpp_count", 0)
    lines.append(
        f"Summary: {bp_count} blueprints, {cpp_count} cpp classes "
        f"({project_cpp_count} project / {engine_cpp_count} engine)."
    )
    lines.append("")

    lines.append("## Hierarchy")
    hierarchy = blueprint_data.get("hierarchy", {})
    rows: list[dict] = []
    _flatten_hierarchy(hierarchy, rows)
    if not rows:
        lines.append("- No hierarchy data available.")
        lines.append("")
        return "\n".join(lines)

    lines.append("| Class | Type | Asset Path |")
    lines.append("|-------|------|------------|")
    for row in rows:
        indent = "  " * int(row.get("depth", 0))
        name = f"{indent}{row.get('name', '')}"
        cls_type = row.get("type", "")
        asset_path = row.get("asset_path", "")
        lines.append(f"| `{name}` | {cls_type} | {asset_path} |")
    lines.append("")
    return "\n".join(lines)


def update_domain_auto_section(content: str, section_name: str, new_content: str) -> str:
    """Replace text between marker pairs, preserving surrounding human-authored content."""
    start_marker = f"<!-- auto:{section_name}:start -->"
    end_marker = f"<!-- auto:{section_name}:end -->"

    start_idx = content.find(start_marker)
    end_idx = content.find(end_marker)

    if start_idx == -1 or end_idx == -1:
        if start_idx != -1 or end_idx != -1:
            logger.warning(
                "Found partial auto markers for '%s' — both start and end required",
                section_name,
            )
        return content

    before = content[: start_idx + len(start_marker)]
    after = content[end_idx:]
    return f"{before}\n{new_content}\n{after}"


def _decode_data(response: dict, fallback=None) -> dict:
    """Decode the 'data' field from a TCP response.

    The TCP protocol wraps payloads as JSON-encoded strings.
    This safely handles both string and already-decoded dict responses.
    """
    if fallback is None:
        fallback = {}
    raw = response.get("data", fallback)
    if raw is None:
        return fallback
    if isinstance(raw, str):
        try:
            decoded = json.loads(raw)
        except (json.JSONDecodeError, TypeError):
            return fallback
        return decoded if isinstance(decoded, dict) else fallback
    return raw if isinstance(raw, dict) else fallback


def _load_pattern_file(path: pathlib.Path) -> list[str]:
    """Load non-empty, non-comment lines from a config file."""
    if not path.exists():
        return []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
        return [line.strip() for line in lines if line.strip() and not line.strip().startswith("#")]
    except OSError:
        return []


def filter_engine_tags(
    tag_prefixes: list[dict],
    excluded_prefixes: set[str] | None = None,
) -> list[dict]:
    """Remove configured GameplayTag prefixes.

    Filtering is opt-in via excluded_prefixes to avoid dropping legitimate project roots.
    """
    if not excluded_prefixes:
        return tag_prefixes
    return [tp for tp in tag_prefixes if tp.get("prefix") not in excluded_prefixes]


def load_schema_excludes(path: pathlib.Path) -> list[str]:
    """Load path exclusion patterns from a config file.

    Each non-empty, non-comment line is a substring to match against asset paths.
    """
    return _load_pattern_file(path)


def load_schema_tag_excludes(path: pathlib.Path) -> list[str]:
    """Load tag prefix exclusions from config file."""
    return _load_pattern_file(path)


def filter_excluded_paths(items: list[dict], excludes: list[str], path_key: str = "path") -> list[dict]:
    """Filter items whose path field contains any exclusion pattern."""
    if not excludes:
        return items
    return [item for item in items if not any(ex in item.get(path_key, "") for ex in excludes)]


def filter_data_asset_classes(data_asset_classes: list[dict], excludes: list[str]) -> list[dict]:
    """Filter DataAsset class summaries using per-asset paths when available.

    If asset_paths are unavailable, preserve the entry to avoid false removals.
    """
    if not excludes:
        return data_asset_classes

    filtered: list[dict] = []
    for entry in data_asset_classes:
        asset_paths = entry.get("asset_paths")
        if isinstance(asset_paths, list):
            included_paths = [
                p for p in asset_paths
                if isinstance(p, str) and not any(ex in p for ex in excludes)
            ]
            if not included_paths:
                continue
            new_entry = dict(entry)
            new_entry["count"] = len(included_paths)
            new_entry["example_path"] = included_paths[0]
            new_entry["asset_paths"] = included_paths
            filtered.append(new_entry)
            continue

        # No per-asset path list available; keep entry to avoid inaccurate dropping.
        filtered.append(entry)
    return filtered


# Engine/Slate types to collapse at depth 1 (type name only, no nested fields)
ENGINE_STRUCT_NAMES = frozenset({
    "SlateBrush", "SlateColor", "SlateFontInfo", "FontOutlineSettings",
    "LinearColor", "Vector2D", "Vector", "Rotator", "Transform",
    "Margin", "InputScaleBias", "InputScaleBiasClamp",
    "RuntimeFloatCurve", "RichCurve",
})


def _is_engine_type(type_name: str) -> bool:
    """Check if a type is an engine/Slate struct that should be collapsed."""
    clean = type_name.removeprefix("F")
    return clean in ENGINE_STRUCT_NAMES or type_name in ENGINE_STRUCT_NAMES


def truncate_nested_fields(
    fields: list[dict],
    max_depth: int = 3,
    engine_collapse_depth: int = 0,
    _current_depth: int = 0,
) -> list[dict]:
    """Truncate nested struct fields based on depth limits.

    max_depth controls how many nesting levels project structs show.
    engine_collapse_depth controls when engine/Slate struct children are collapsed.
    A value of 0 means collapse immediately (type name only, no children shown).
    A field at _current_depth expands its children only if _current_depth < limit,
    so max_depth=3 shows children at depths 0, 1, 2 (3 levels visible).
    """
    result = []
    for field in fields:
        field_copy = {k: v for k, v in field.items() if k != "fields"}
        nested = field.get("fields")
        if nested:
            type_name = field.get("type", field.get("cpp_type", ""))
            if _is_engine_type(type_name):
                if _current_depth < engine_collapse_depth:
                    field_copy["fields"] = truncate_nested_fields(
                        nested, max_depth, engine_collapse_depth, _current_depth + 1
                    )
                # else: omit fields (collapsed)
            elif _current_depth < max_depth - 1:
                field_copy["fields"] = truncate_nested_fields(
                    nested, max_depth, engine_collapse_depth, _current_depth + 1
                )
            # else: omit fields (depth exceeded)
        result.append(field_copy)
    return result


def render_data_index(catalog: dict) -> str:
    """Render data/_index.md — table listing grouped by struct type.

    Composite tables listed separately with source tables.
    """
    lines = ["# Data Domain Index", "", _render_meta("data-index"), ""]

    tables = catalog.get("datatables", [])
    regular = [t for t in tables if not t.get("is_composite")]
    composites = [t for t in tables if t.get("is_composite")]

    # Group regular tables by struct
    struct_groups: dict[str, list[dict]] = {}
    for t in regular:
        struct_groups.setdefault(t.get("row_struct", "Unknown"), []).append(t)

    for struct_name, group in struct_groups.items():
        total_rows = sum(t["row_count"] for t in group)
        lines.append(f"## {struct_name} ({len(group)} tables, {total_rows} rows)")
        table_list = " ".join(f"{t['name']}({t['row_count']})" for t in group)
        lines.append(table_list)
        lines.append("")

    # Composite tables
    if composites:
        lines.append("## Composites")
        for t in composites:
            parents = t.get("parent_tables", [])
            parent_names = ", ".join(
                p["name"] if isinstance(p, dict) else p.rsplit("/", 1)[-1]
                for p in parents
            )
            lines.append(f"{t['name']}({t['row_count']}) <- {parent_names}")
        lines.append("")

    # Tag prefixes
    tag_prefixes = catalog.get("tag_prefixes", [])
    if tag_prefixes:
        lines.append("## Tags")
        for tp in tag_prefixes:
            lines.append(f"- {tp['prefix']} ({tp['count']})")
        lines.append("")

    # DataAsset classes
    asset_classes = catalog.get("data_asset_classes", [])
    if asset_classes:
        lines.append("## DataAssets")
        for ac in asset_classes:
            lines.append(f"- {ac['class_name']} ({ac['count']})")
        lines.append("")

    # StringTables
    string_tables = catalog.get("string_tables", [])
    if string_tables:
        lines.append("## StringTables")
        for st in string_tables:
            lines.append(f"- {st['name']} ({st.get('entry_count', '?')} entries)")
        lines.append("")

    return "\n".join(lines)


def _compact_field(field: dict, indent: int = 2) -> list[str]:
    """Render a field in compact notation: `Name: Type (default: X)`."""
    prefix = " " * indent
    type_name = field.get("type", field.get("cpp_type", "unknown"))
    parts = [f"{prefix}- {field.get('name', '<unknown>')}: {type_name}"]
    if field.get("default_value"):
        parts[0] += f" (default: {field['default_value']})"
    if field.get("enum_values"):
        vals = ", ".join(field["enum_values"])
        parts[0] += f" [{vals}]"
    if field.get("element_type"):
        elem = field["element_type"]
        if isinstance(elem, dict):
            parts[0] += f"<{elem.get('type', elem.get('cpp_type', ''))}>"
    lines = parts
    if field.get("fields"):
        for sub in field["fields"]:
            lines.extend(_compact_field(sub, indent + 2))
    return lines


def render_data_structs(schemas: dict[str, dict]) -> str:
    """Render data/structs.md — struct field definitions with depth limiting."""
    lines = ["# Data Struct Schemas", "", _render_meta("data-structs"), ""]

    for struct_name, schema_data in schemas.items():
        parent = schema_data.get("parent", "FTableRowBase")
        lines.append(f"## {struct_name} (extends {parent})")
        raw_fields = schema_data.get("schema", [])
        # Apply depth limiting before rendering
        fields = truncate_nested_fields(raw_fields, max_depth=3, engine_collapse_depth=0)
        for field in fields:
            lines.extend(_compact_field(field))
        lines.append("")

    return "\n".join(lines)


def collect_format_examples(
    catalog: dict,
    example_rows: dict[str, list],
) -> dict[str, dict]:
    """Collect 1 format example per unique struct type. Skip composites."""
    seen_structs: dict[str, dict] = {}
    for table in catalog.get("datatables", []):
        if table.get("is_composite"):
            continue
        struct_name = table.get("row_struct", "Unknown")
        if struct_name in seen_structs:
            continue
        rows = example_rows.get(table["name"], [])
        if rows:
            row = rows[0]
            seen_structs[struct_name] = {
                "source_table": table["name"],
                "row_data": row.get("row_data", {}),
            }
    return seen_structs


def _truncate_value(value: Any, max_items: int = 3) -> str:
    """Format a value for display, truncating long arrays."""
    if isinstance(value, list):
        if len(value) > max_items:
            shown = ", ".join(str(v) for v in value[:max_items])
            return f"[{shown}, ...(+{len(value) - max_items} more)]"
        return json.dumps(value)
    return f"`{value}`"


def render_data_formats(format_examples: dict[str, dict]) -> str:
    """Render data/formats.md — 1 compact format example per unique struct."""
    lines = ["# Data Format Examples", "", _render_meta("data-formats"), ""]

    if not format_examples:
        lines.append("No format examples available.")
        lines.append("")
        return "\n".join(lines)

    for struct_name, example in format_examples.items():
        source = example.get("source_table", "unknown")
        row_data = example.get("row_data", {})
        lines.append(f"## {struct_name}")
        lines.append(f"Source: {source}")
        if row_data:
            lines.append("| Field | Example |")
            lines.append("|-------|---------|")
            for field_name, value in row_data.items():
                lines.append(f"| {field_name} | {_truncate_value(value)} |")
        lines.append("")

    return "\n".join(lines)


def collect_data_domain(connection, project_root: pathlib.Path | None = None) -> dict:
    """Collect all data domain information from a live UE editor.

    Calls existing TCP commands and assembles the raw data.

    Args:
        connection: UEConnection instance (connected to running editor).
        project_root: Project root directory used to locate .cortex/config/schema_excludes.txt.
            Pass None to skip path exclusion.

    Returns:
        Dict with catalog, schemas, example_rows, curve_tables, enum_values, summary.
    """
    # 1. Get catalog
    catalog_resp = connection.send_command("data.get_data_catalog", {})
    catalog = _decode_data(catalog_resp)
    excludes: list[str] = []
    tag_excludes: list[str] = []
    if project_root is not None:
        config_dir = project_root / ".cortex" / "config"
        excludes = load_schema_excludes(config_dir / "schema_excludes.txt")
        tag_excludes = load_schema_tag_excludes(config_dir / "schema_tag_excludes.txt")

    catalog["tag_prefixes"] = filter_engine_tags(
        catalog.get("tag_prefixes", []),
        excluded_prefixes=set(tag_excludes),
    )

    # Apply user-configured path exclusions
    if excludes:
        catalog["datatables"] = filter_excluded_paths(catalog.get("datatables", []), excludes)
        catalog["string_tables"] = filter_excluded_paths(
            catalog.get("string_tables", []),
            excludes,
        )
        catalog["data_asset_classes"] = filter_data_asset_classes(
            catalog.get("data_asset_classes", []),
            excludes,
        )

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
                schema_data = _decode_data(resp)
                raw_schema = schema_data.get("schema", [])
                # TCP may return schema as {"struct_name":..,"fields":[..]}
                if isinstance(raw_schema, dict):
                    fields = raw_schema.get("fields", [])
                else:
                    fields = raw_schema
                schemas[struct_name] = {
                    "struct_name": struct_name,
                    "parent": schema_data.get("parent_struct", "FTableRowBase"),
                    "schema": fields,
                }
            except (RuntimeError, ConnectionError) as e:
                logger.warning("Failed to get schema for %s: %s", struct_name, e)

    # 3. Collect 1 example row per unique struct (not per table), skip composites
    example_rows = {}
    seen_structs_for_examples: set[str] = set()
    for table in catalog.get("datatables", []):
        if table.get("is_composite"):
            continue
        if table["row_struct"] in seen_structs_for_examples:
            continue
        try:
            resp = connection.send_command(
                "data.query_datatable",
                {"table_path": table["path"], "limit": 1, "offset": 0},
            )
            query_result = _decode_data(resp)
            rows = query_result.get("rows", [])
            if rows:
                example_rows[table["name"]] = rows[:1]
                seen_structs_for_examples.add(table["row_struct"])
        except (RuntimeError, ConnectionError) as e:
            logger.warning("Failed to get example rows for %s: %s", table["name"], e)

    # 4. Get curve tables
    curve_tables = []
    try:
        resp = connection.send_command("data.list_curve_tables", {})
        curve_result = _decode_data(resp)
        curve_tables = curve_result.get("curve_tables", [])
        if excludes:
            curve_tables = filter_excluded_paths(curve_tables, excludes)
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

    # 7. Compute format examples (1 per unique struct)
    format_examples = collect_format_examples(catalog, example_rows)

    return {
        "catalog": catalog,
        "schemas": schemas,
        "example_rows": example_rows,
        "format_examples": format_examples,
        "curve_tables": curve_tables,
        "enum_values": enum_values,
        "summary": summary,
    }


def generate_schema(
    connection,
    schema_dir: pathlib.Path,
    domain: str = "all",
    project_name: str = "Unknown",
    engine_version: str = "",
    plugin_version: str = "",
) -> dict:
    """Generate schema files in the given directory.

    Args:
        connection: UEConnection instance.
        schema_dir: Target directory (e.g., .cortex/schema/).
        domain: "all" or specific domain name ("data", "blueprints").
        project_name: Project name for catalog header.
        engine_version: Unreal Engine version (e.g., "5.6").
        plugin_version: UnrealCortex plugin version (e.g., "1.0.0").

    Returns:
        Dict with generated domain names and file paths.
    """
    # Fetch engine/plugin version from editor if not provided
    if not engine_version or not plugin_version:
        try:
            status = connection.send_command("get_status", {})
            status_data = _decode_data(status)
            if not engine_version:
                engine_version = status_data.get("engine_version", "")
            if not plugin_version:
                plugin_version = status_data.get("plugin_version", "")
        except (RuntimeError, ConnectionError) as e:
            logger.warning("Failed to get editor status for version info: %s", e)

    result = {"generated": {}, "errors": []}
    data_summary = None
    blueprint_summary = None

    if domain in ("all", "data"):
        try:
            collected = collect_data_domain(connection, project_root=find_project_root())

            # Write data/_index.md
            index_md = render_data_index(collected["catalog"])
            atomic_write(schema_dir / "data" / "_index.md", index_md)
            result["generated"]["data_index"] = str(schema_dir / "data" / "_index.md")

            # Write data/structs.md
            structs_md = render_data_structs(collected["schemas"])
            atomic_write(schema_dir / "data" / "structs.md", structs_md)
            result["generated"]["data_structs"] = str(schema_dir / "data" / "structs.md")

            # Write data/formats.md
            formats_md = render_data_formats(collected["format_examples"])
            atomic_write(schema_dir / "data" / "formats.md", formats_md)
            result["generated"]["data_formats"] = str(schema_dir / "data" / "formats.md")

            data_summary = collected["summary"]

            # Clean up old v1 monolithic file
            old_data_md = schema_dir / "data.md"
            if old_data_md.exists():
                old_data_md.unlink()
        except (ConnectionError, RuntimeError) as e:
            logger.error("Failed to generate data schema: %s", e)
            result["errors"].append(f"data: {e}")
            raise
        except Exception as e:
            logger.error("Failed to generate data schema: %s", e)
            result["errors"].append(f"data: {e}")
            raise RuntimeError(f"Failed to generate data schema: {e}") from e

    if domain in ("all", "blueprints"):
        try:
            blueprint_summary = collect_blueprint_domain(connection)
            blueprints_md = render_blueprint_catalog(blueprint_summary)
            atomic_write(schema_dir / "blueprints.md", blueprints_md)
            result["generated"]["blueprints"] = str(schema_dir / "blueprints.md")

            project_root = find_project_root()
            domain_file = project_root / ".cortex" / "domains" / "blueprints.md"
            if domain_file.exists():
                original = domain_file.read_text(encoding="utf-8")
                auto_stats = (
                    f"**Blueprints:** {blueprint_summary.get('blueprint_count', 0)} total, "
                    f"{blueprint_summary.get('project_cpp_count', 0)} project cpp, "
                    f"{blueprint_summary.get('engine_cpp_count', 0)} engine cpp"
                )
                updated = update_domain_auto_section(original, "blueprint-stats", auto_stats)
                if updated != original:
                    atomic_write(domain_file, updated)
                    result["generated"]["blueprint_domain"] = str(domain_file)
        except (ConnectionError, RuntimeError) as e:
            logger.error("Failed to generate blueprint schema: %s", e)
            result["errors"].append(f"blueprints: {e}")
            raise
        except Exception as e:
            logger.error("Failed to generate blueprint schema: %s", e)
            result["errors"].append(f"blueprints: {e}")
            raise RuntimeError(f"Failed to generate blueprint schema: {e}") from e

    # Always regenerate catalog
    catalog_md = render_catalog(
        project_name=project_name,
        data_summary=data_summary,
        blueprint_summary=blueprint_summary,
        engine_version=engine_version,
        plugin_version=plugin_version,
    )
    atomic_write(schema_dir / "_catalog.md", catalog_md)
    result["generated"]["_catalog"] = str(schema_dir / "_catalog.md")

    return result
