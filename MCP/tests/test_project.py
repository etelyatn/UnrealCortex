"""Unit tests for project directory resolution."""

import os
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from cortex_mcp.project import (
    resolve_project_dir,
    resolve_saved_dir,
    _walk_up_for_uproject,
)


@pytest.fixture(autouse=True)
def clear_lru_cache():
    """Clear the cached walk-up result between tests."""
    _walk_up_for_uproject.cache_clear()
    yield
    _walk_up_for_uproject.cache_clear()


class TestResolveProjectDir:
    """Tests for resolve_project_dir()."""

    def test_absolute_cortex_project_dir(self, tmp_path):
        """Absolute CORTEX_PROJECT_DIR is returned as-is."""
        project = tmp_path / "MyProject"
        project.mkdir()
        with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": str(project)}, clear=False):
            # Clear CLAUDE_PROJECT_DIR to isolate test
            env = {"CORTEX_PROJECT_DIR": str(project)}
            with patch.dict(os.environ, env, clear=False):
                os.environ.pop("CLAUDE_PROJECT_DIR", None)
                result = resolve_project_dir()
                assert result == project

    def test_relative_cortex_project_dir_with_walk_up(self, tmp_path):
        """Relative CORTEX_PROJECT_DIR resolves against walk-up root."""
        # Mock walk-up to return tmp_path
        with patch("cortex_mcp.project._walk_up_for_uproject", return_value=tmp_path):
            with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": "."}, clear=False):
                os.environ.pop("CLAUDE_PROJECT_DIR", None)
                result = resolve_project_dir()
                assert result == tmp_path

    def test_relative_cortex_project_dir_falls_back_to_claude_dir(self, tmp_path):
        """Relative CORTEX_PROJECT_DIR falls back to CLAUDE_PROJECT_DIR when walk-up fails."""
        project = tmp_path / "GameProject"
        project.mkdir()
        with patch("cortex_mcp.project._walk_up_for_uproject", return_value=None):
            env = {
                "CORTEX_PROJECT_DIR": ".",
                "CLAUDE_PROJECT_DIR": str(project),
            }
            with patch.dict(os.environ, env, clear=False):
                result = resolve_project_dir()
                assert result == project

    def test_relative_cortex_project_dir_no_fallback_returns_none(self):
        """Relative CORTEX_PROJECT_DIR with no fallback returns None."""
        with patch("cortex_mcp.project._walk_up_for_uproject", return_value=None):
            with patch.dict(os.environ, {"CORTEX_PROJECT_DIR": "."}, clear=False):
                os.environ.pop("CLAUDE_PROJECT_DIR", None)
                result = resolve_project_dir()
                assert result is None

    def test_claude_project_dir_fallback(self, tmp_path):
        """CLAUDE_PROJECT_DIR used when CORTEX_PROJECT_DIR not set and .uproject present."""
        project = tmp_path / "GameProject"
        project.mkdir()
        (project / "MyGame.uproject").touch()
        with patch("cortex_mcp.project._walk_up_for_uproject", return_value=None):
            with patch.dict(os.environ, {"CLAUDE_PROJECT_DIR": str(project)}, clear=False):
                os.environ.pop("CORTEX_PROJECT_DIR", None)
                result = resolve_project_dir()
                assert result == project

    def test_claude_project_dir_without_uproject_falls_through(self, tmp_path):
        """CLAUDE_PROJECT_DIR without *.uproject is skipped; walk-up is used instead."""
        project = tmp_path / "NotUnreal"
        project.mkdir()
        walk_up_result = tmp_path / "FallbackProject"
        walk_up_result.mkdir()
        with patch("cortex_mcp.project._walk_up_for_uproject", return_value=walk_up_result):
            with patch.dict(os.environ, {"CLAUDE_PROJECT_DIR": str(project)}, clear=False):
                os.environ.pop("CORTEX_PROJECT_DIR", None)
                result = resolve_project_dir()
                assert result == walk_up_result

    def test_walk_up_fallback(self, tmp_path):
        """Walk-up is used when no env vars set."""
        with patch("cortex_mcp.project._walk_up_for_uproject", return_value=tmp_path):
            env_clean = {}
            with patch.dict(os.environ, env_clean, clear=False):
                os.environ.pop("CORTEX_PROJECT_DIR", None)
                os.environ.pop("CLAUDE_PROJECT_DIR", None)
                result = resolve_project_dir()
                assert result == tmp_path

    def test_no_resolution_returns_none(self):
        """Returns None when all resolution methods fail."""
        with patch("cortex_mcp.project._walk_up_for_uproject", return_value=None):
            with patch.dict(os.environ, {}, clear=False):
                os.environ.pop("CORTEX_PROJECT_DIR", None)
                os.environ.pop("CLAUDE_PROJECT_DIR", None)
                result = resolve_project_dir()
                assert result is None


class TestResolveSavedDir:
    """Tests for resolve_saved_dir()."""

    def test_returns_saved_subdir(self, tmp_path):
        """Returns project_root/Saved when it exists."""
        saved = tmp_path / "Saved"
        saved.mkdir()
        with patch("cortex_mcp.project.resolve_project_dir", return_value=tmp_path):
            result = resolve_saved_dir()
            assert result == saved

    def test_returns_none_when_saved_missing(self, tmp_path):
        """Returns None when Saved/ doesn't exist."""
        with patch("cortex_mcp.project.resolve_project_dir", return_value=tmp_path):
            result = resolve_saved_dir()
            assert result is None

    def test_returns_none_when_no_project(self):
        """Returns None when project dir can't be resolved."""
        with patch("cortex_mcp.project.resolve_project_dir", return_value=None):
            result = resolve_saved_dir()
            assert result is None


class TestWalkUpForUproject:
    """Tests for _walk_up_for_uproject()."""

    def test_finds_uproject_in_ancestor(self):
        """Should find the .uproject file in an ancestor directory."""
        # This test runs against the actual repo — the .uproject should exist
        result = _walk_up_for_uproject()
        if result is not None:
            uprojects = list(result.glob("*.uproject"))
            assert len(uprojects) > 0
