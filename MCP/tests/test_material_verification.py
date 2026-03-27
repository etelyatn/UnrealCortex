"""Tests for material verification helpers."""

import pytest


def test_verification_inconclusive_when_nodes_empty_but_spec_has_nodes():
    """If readback returns 0 nodes but spec expects nodes, result should be
    inconclusive (verified=None) not a failure (verified=False)."""
    from cortex_mcp.verification.material import verify_material

    spec = {
        "nodes": [
            {"class": "VectorParameter", "name": "BaseColor"},
        ],
        "connections": [
            {"from": "BaseColor.0", "to": "Material.BaseColor"},
        ],
    }
    readback = {
        "node_count": 0,
        "nodes": [],         # empty — asset registry hasn't synced yet
        "connections": [],
        "blend_mode": None,
        "shading_model": None,
    }

    result = verify_material(spec, readback)
    # Should NOT be verified=False when readback is empty (timing race)
    # Should be verified=None (inconclusive)
    assert result.verified is not False, (
        f"Verification should be inconclusive (None) when readback is empty, "
        f"got verified={result.verified}"
    )
