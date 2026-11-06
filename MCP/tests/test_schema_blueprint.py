"""Tests for blueprint domain schema generation."""

import re
from unittest.mock import MagicMock

import pytest

from cortex_mcp.schema_generator import (
    SCHEMA_VERSION,
    generate_schema,
    read_meta_from_file,
    read_schema_metadata,
    render_blueprint_catalog,
    render_catalog,
    update_domain_auto_section,
)

# Fixture identities for the offline catalog: a native parent/child pair plus one saved
# Blueprint child, so the authoritative file and the entry point can be told apart by identity.
CATALOG_ROOT_CLASS = "AActor"
CATALOG_NATIVE_CLASS = "ZActor"
CATALOG_CHILD_CLASS = "BP_CatalogChild_C"
CATALOG_CHILD_ASSET_PATH = "/Game/Catalog/BP_CatalogChild.BP_CatalogChild"


def _blueprint_link_targets(catalog_text: str) -> list[str]:
    """Relative markdown link targets in a catalog file (navigation, not heading prose)."""
    return [match.strip().lstrip("./") for match in re.findall(r"\]\(([^)]+)\)", catalog_text)]


def _make_blueprint_connection():
    """Finite fixture connection serving the real commands blueprint collection uses."""
    connection = MagicMock()
    native_data = {
        "name": CATALOG_ROOT_CLASS,
        "type": "cpp",
        "children": [{"name": CATALOG_NATIVE_CLASS, "type": "cpp", "children": []}],
        "cpp_count": 2,
        "blueprint_count": 1,
        "project_cpp_count": 1,
        "engine_cpp_count": 1,
    }
    catalog_data = {
        "complete": True,
        "blueprint_count": 1,
        "classes": [
            {
                "name": CATALOG_CHILD_CLASS,
                "generated_class_path": "/Game/Catalog/BP_CatalogChild.BP_CatalogChild_C",
                "parent_name": CATALOG_NATIVE_CLASS,
                "parent_class_path": "/Script/Project.ZActor",
                "native_parent_class_path": "/Script/Project.ZActor",
                "asset_path": CATALOG_CHILD_ASSET_PATH,
            },
        ],
    }

    def send(command, params=None, ttl=None):
        if command == "get_status":
            return {"success": True, "data": {"engine_version": "5.8", "plugin_version": "0.1.0"}}
        if command == "reflect.class_hierarchy":
            return {"success": True, "data": native_data}
        if command == "reflect.blueprint_catalog":
            return {"success": True, "data": catalog_data}
        raise AssertionError(f"Unexpected command: {command}")

    connection.send_command.side_effect = send
    connection.send_command_cached.side_effect = send
    return connection


@pytest.fixture
def generated_blueprint_schema(tmp_path):
    """Generate blueprints.md and _catalog.md through the real generation path."""
    schema_dir = tmp_path / ".cortex" / "schema"
    result = generate_schema(
        _make_blueprint_connection(),
        schema_dir,
        domain="blueprints",
        project_name="CortexSandbox",
        project_root=tmp_path,
    )
    return schema_dir, result


def test_render_blueprint_catalog_basic():
    """Blueprint catalog should render flat rows with direct parent links."""
    blueprint_data = {
        "classes": [
            {
                "name": "BP_TestActor_C",
                "type": "blueprint",
                "parent": "AActor",
                "generated_class_path": "/Game/Blueprints/BP_TestActor.BP_TestActor_C",
                "asset_path": "/Game/Blueprints/BP_TestActor.BP_TestActor",
            },
            {"name": "AActor", "type": "cpp", "parent": "UObject", "asset_path": ""},
        ],
        "blueprint_count": 1,
        "cpp_count": 1,
        "project_cpp_count": 1,
        "engine_cpp_count": 0,
    }

    result = render_blueprint_catalog(blueprint_data)
    assert "# Blueprint Catalog" in result
    assert "| Class | Type | Parent | Asset Path |" in result
    assert "| `BP_TestActor_C` | blueprint | AActor | /Game/Blueprints/BP_TestActor.BP_TestActor |" in result
    assert "AActor" in result


def test_render_blueprint_catalog_is_stable_for_shuffled_rows():
    first = {
        "classes": [
            {
                "name": "BP_Zed_C",
                "type": "blueprint",
                "parent": "AActor",
                "generated_class_path": "/Game/Zed.BP_Zed_C",
                "asset_path": "/Game/Zed.Zed",
            },
            {"name": "ZActor", "type": "cpp", "parent": "AActor", "asset_path": ""},
            {
                "name": "BP_Alpha_C",
                "type": "blueprint",
                "parent": "ZActor",
                "generated_class_path": "/Game/Alpha.BP_Alpha_C",
                "asset_path": "/Game/Alpha.Alpha",
            },
            {"name": "AActor", "type": "cpp", "parent": "UObject", "asset_path": ""},
        ],
        "blueprint_count": 2,
        "cpp_count": 2,
        "project_cpp_count": 2,
        "engine_cpp_count": 0,
    }
    second = {**first, "classes": list(reversed(first["classes"]))}

    first_markdown = render_blueprint_catalog(first)
    second_markdown = render_blueprint_catalog(second)

    assert first_markdown == second_markdown
    assert first_markdown.index("`AActor`") < first_markdown.index("`ZActor`")
    assert first_markdown.index("`BP_Alpha_C`") < first_markdown.index("`BP_Zed_C`")
    assert "generated:" not in first_markdown


def test_render_blueprint_catalog_empty():
    """Empty hierarchy should still render valid markdown."""
    blueprint_data = {
        "hierarchy": {"name": "AActor", "type": "cpp", "children": []},
        "blueprint_count": 0,
        "cpp_count": 0,
        "project_cpp_count": 0,
        "engine_cpp_count": 0,
    }

    result = render_blueprint_catalog(blueprint_data)
    assert "# Blueprint Catalog" in result
    assert "0 blueprints" in result


def test_catalog_links_blueprint_entry_without_class_table():
    """_catalog.md stays an entry point: a relative link plus counts, no class rows."""
    blueprint_summary = {
        "classes": [
            {
                "name": "BP_Test_C",
                "type": "blueprint",
                "parent": "AActor",
                "asset_path": "/Game/BP_Test.BP_Test",
            },
            {"name": "AActor", "type": "cpp", "parent": "UObject", "asset_path": ""},
        ],
        "blueprint_count": 1,
    }

    result = render_catalog(project_name="TestProject", blueprint_summary=blueprint_summary)

    assert "blueprints.md" in _blueprint_link_targets(result)
    assert "BP_Test_C" not in result

    overview_rows = [
        line
        for line in result.splitlines()
        if line.lstrip().startswith("|") and "blueprint" in line.lower()
    ]
    assert overview_rows, "catalog must keep the blueprint overview row"
    assert any(
        str(count) in row
        for row in overview_rows
        for count in (blueprint_summary["blueprint_count"], len(blueprint_summary["classes"]))
    ), "the blueprint overview row must report class counts"


def test_offline_catalog_links_to_the_blueprint_file(generated_blueprint_schema):
    """The offline entry point resolves to the authoritative blueprint file."""
    schema_dir, result = generated_blueprint_schema

    assert result["errors"] == []
    assert "blueprints" in result["generated"]
    assert "_catalog" in result["generated"]

    catalog = schema_dir / "_catalog.md"
    assert catalog.is_file()

    targets = _blueprint_link_targets(catalog.read_text(encoding="utf-8"))
    assert "blueprints.md" in targets, "catalog must link to blueprints.md"

    linked = catalog.parent / "blueprints.md"
    assert linked.is_file()


def test_offline_blueprint_catalog_owns_class_identity(generated_blueprint_schema):
    """Class identity lives in blueprints.md; _catalog.md repeats none of it."""
    schema_dir, _ = generated_blueprint_schema
    catalog = schema_dir / "_catalog.md"
    authoritative = catalog.parent / "blueprints.md"

    blueprint_text = authoritative.read_text(encoding="utf-8")
    assert (
        f"| `{CATALOG_CHILD_CLASS}` | blueprint | {CATALOG_NATIVE_CLASS} | "
        f"{CATALOG_CHILD_ASSET_PATH} |"
    ) in blueprint_text
    rows = re.findall(r"^\| `([^`]+)` \| ", blueprint_text, re.MULTILINE)
    assert set(rows) == {CATALOG_ROOT_CLASS, CATALOG_NATIVE_CLASS, CATALOG_CHILD_CLASS}

    catalog_text = catalog.read_text(encoding="utf-8")
    for name in rows:
        assert name not in catalog_text, f"class {name} is duplicated in _catalog.md"
    assert CATALOG_CHILD_ASSET_PATH not in catalog_text

    metadata = read_schema_metadata(schema_dir)
    assert {"blueprints.md", "_catalog.md"} <= set(metadata["files"])
    blueprint_meta = read_meta_from_file(authoritative)
    assert blueprint_meta is not None
    assert blueprint_meta["schema_version"] == str(SCHEMA_VERSION)
    assert blueprint_meta["domain"] == "blueprints"
    catalog_meta = read_meta_from_file(catalog)
    assert catalog_meta is not None
    assert catalog_meta["schema_version"] == str(SCHEMA_VERSION)
    assert catalog_meta["domain"] == "catalog"
    assert catalog_meta["engine"] == "5.8"
    assert catalog_meta["plugin"] == "0.1.0"


def test_update_domain_file_markers():
    """Should update content between auto markers without touching human content."""
    original = """# Blueprint Domain

Human-authored content here.

<!-- auto:blueprint-stats:start -->
Old auto content
<!-- auto:blueprint-stats:end -->

More human content below.
"""

    new_auto_content = "**Blueprints:** 15 total, 10 actors, 5 widgets"
    result = update_domain_auto_section(original, "blueprint-stats", new_auto_content)

    assert "Human-authored content here." in result
    assert "More human content below." in result
    assert "15 total" in result
    assert "Old auto content" not in result


def test_update_domain_file_no_markers():
    """If no markers exist, content should be unchanged."""
    original = "# Blueprint Domain\n\nNo markers here.\n"
    result = update_domain_auto_section(original, "blueprint-stats", "new content")
    assert result == original


def test_update_domain_file_partial_markers():
    """If only one marker exists, content should be unchanged."""
    original = "# Blueprint Domain\n\n<!-- auto:blueprint-stats:start -->\nContent\n"
    result = update_domain_auto_section(original, "blueprint-stats", "new content")
    assert result == original
