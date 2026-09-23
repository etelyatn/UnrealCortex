"""Tests for blueprint domain schema generation."""

from cortex_mcp.schema_generator import (
    render_blueprint_catalog,
    render_catalog,
    update_domain_auto_section,
)


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


def test_catalog_includes_blueprint_summary():
    """Main catalog should include blueprint row when summary provided."""
    bp_summary = {"classes": [{"name": "BP_Test", "parent": "AActor"}]}
    result = render_catalog(
        project_name="TestProject",
        blueprint_summary=bp_summary,
    )
    assert "blueprints" in result.lower()
    assert "BP_Test" in result


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
