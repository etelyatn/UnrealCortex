"""Bind the cortex-toolkit intent fixtures to the live editor (T15 R8 / toolkit TK03).

The fixtures under `cortex-toolkit/examples/typed-blueprint-authoring/` are executable intent
fragments: they carry only envelope fields and write every value the Editor must decide as a
`<live: ...>` token. This module loads those files from the submodule path, binds their tokens to the
live editor through the registered facade entrypoints and asserts the contract the fixtures and their
README declare.

Unproducible tokens are named explicitly below with their reason instead of being silently ignored:
`test_toolkit_fixture_tokens_are_either_bound_or_declared` fails if a fixture declares a token that is
neither bound by this module nor listed here, so a new fixture token cannot slip through unnoticed.

Run (editor lease required):
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_graph_authoring_toolkit_examples.py -m scenario -v
"""

from __future__ import annotations

import json
import uuid
from pathlib import Path

import pytest

from test_graph_authoring_scenario import (
    RECORDING_DEFAULT_LITERAL,
    TITLE_ARGUMENT,
    AuthoringRun,
    build_adapter,
    build_host_and_model,
    call,
    describe_node,
    envelope,
    edges,
    node_name_map,
)

pytestmark = pytest.mark.scenario

TOOLKIT_ROOT = (
    Path(__file__).resolve().parents[4] / "cortex-toolkit" / "examples" / "typed-blueprint-authoring"
)

# Tokens this module binds. Anything outside this set must appear in UNPRODUCIBLE below.
BOUND_TOKENS = {
    "run",
    "asset_path",
    "the reviewed graph.apply_patch envelope",
    "source graph_guid",
    "destination graph_guid",
    "selected node GUID",
    "first selected node GUID",
    "second selected node GUID",
    "selected source node GUID",
    "existing destination node GUID",
    "destination node GUID",
    "stale entry node_guid",
    "island entry node_guid",
    "legacy node spec",
    "legacy connection spec",
    "cast target display name",
    "stale entry input pin",
    "stale entry output pin",
    "replacement entry input pin",
    "replacement entry output pin",
    "crossing source pin name",
    "destination pin name",
    "struct-expanded destination pin",
    "validation_hash returned by the preview",
    "a token from a preview of an earlier state",
    "removable set published by the preview",
}

# Declared, deliberately unexercised, with the reason. These are reported in the T15 verification
# document as unverified rows; they are not silently skipped.
UNPRODUCIBLE = {
    "struct-expanded destination pin": (
        "a struct-expanded pin needs a split pin (UEdGraphSchema_K2::SplitPin, a non-UFUNCTION schema "
        "method); no registered command and no editor.run_python entry point can create that "
        "precondition on a generic authored asset, so the expanded-boundary-pin negative is not "
        "provably reachable through the published surface (unproven, not refuted)"
    ),
}


def load_fixture(relative_path: str) -> dict:
    return json.loads((TOOLKIT_ROOT / relative_path).read_text(encoding="utf-8"))


def collect_tokens(value) -> set[str]:
    """Every `<live: ...>` token declared by a fixture fragment."""
    found: set[str] = set()
    if isinstance(value, str):
        if value.startswith("<live:") and value.endswith(">"):
            found.add(value[len("<live:") : -1].strip())
    elif isinstance(value, dict):
        for entry in value.values():
            found |= collect_tokens(entry)
    elif isinstance(value, list):
        for entry in value:
            found |= collect_tokens(entry)
    return found


def bind(template, bindings: dict[str, object]):
    """Rebuilds a fixture fragment with its live tokens replaced by live values."""
    if isinstance(template, str):
        if template.startswith("<live:") and template.endswith(">"):
            token = template[len("<live:") : -1].strip()
            if token not in bindings:
                raise AssertionError(f"fixture token '{token}' has no live binding")
            return bindings[token]
        return template
    if isinstance(template, dict):
        return {key: bind(value, bindings) for key, value in template.items()}
    if isinstance(template, list):
        return [bind(value, bindings) for value in template]
    return template


def test_toolkit_fixture_tokens_are_either_bound_or_declared():
    """Every declared token is bound by this module or named as unproducible, and every one is listed."""
    declared: set[str] = set()
    for path in sorted(TOOLKIT_ROOT.rglob("*.json")):
        declared |= collect_tokens(load_fixture(str(path.relative_to(TOOLKIT_ROOT))))
    assert declared, "the toolkit fixtures declare no live tokens; the loader is wrong"
    unbound = declared - BOUND_TOKENS - set(UNPRODUCIBLE)
    assert not unbound, f"fixture tokens with no binding and no declared reason: {sorted(unbound)}"
    stale_bindings = BOUND_TOKENS - declared
    assert not stale_bindings, f"bindings that no fixture declares any more: {sorted(stale_bindings)}"


async def bind_adapter_flow(client) -> dict:
    """Builds the generic fixture and binds `adapter-intent.json` for the Widget Blueprint context."""
    run = AuthoringRun(client, uuid.uuid4().hex[:8])
    host = await build_host_and_model(run)
    fixture = await build_adapter(run, host)
    return {"run": run, "host": host, **fixture}


@pytest.mark.anyio
async def test_toolkit_adapter_flow_binds_live(mcp_client):
    """`adapter-intent.json` + `adapter-apply.json` bound live, including the README's phase table."""
    context = await bind_adapter_flow(mcp_client)
    run = context["run"]
    try:
        intent = load_fixture("adapter-intent.json")
        overlay = load_fixture("adapter-apply.json")
        fingerprint = (await run.context(context["adapter_package"]))["fingerprint"]

        bound_intent = bind(intent, {"cast target display name": context["cast_result_pin"]})
        # The fragment carries no target and no patch_id: the harness supplies the envelope facts.
        patch_id = str(uuid.uuid4())
        body = envelope(context["adapter_object"], patch_id, fingerprint, **bound_intent)
        body["target"] = context["intent"]["target"]

        preview = await run.apply(body)
        # README "Expected phases" / Preview row.
        assert preview["changed"] is True, preview
        for phase in ("apply_status", "compile_status", "readback_status", "save_status"):
            assert preview[phase] == "not_requested", (phase, preview)
        assert preview["validation_hash"]
        assert set(preview["node_mappings"]) == {node["client_id"] for node in intent["nodes"]}
        assert preview["fingerprint_after"] == preview["fingerprint_before"]

        # `adapter-apply.json` is an overlay of the same fragment, not a second request.
        bound_overlay = bind(overlay, {"validation_hash returned by the preview": preview["validation_hash"]})
        applied = await run.apply({**body, **bound_overlay})
        assert applied["apply_status"] == "applied", applied
        assert applied["compile_status"] == "compiled", applied
        assert applied["readback_status"] == "matched", applied
        assert applied["target_compile_count"] == 1, applied
        assert applied["save_status"] == "not_requested", applied
        assert applied["dirty_after"] is True and applied["blocked"] is False, applied

        # Repeat with the same patch_id: the README's replay row.
        replay_preview = await run.apply(
            envelope(
                context["adapter_object"],
                patch_id,
                (await run.context(context["adapter_package"]))["fingerprint"],
                **bound_intent,
                target=context["intent"]["target"],
            )
        )
        assert replay_preview["changed"] is False, replay_preview
        replay = await run.apply(
            {**body, "dry_run": False, "expected_validation_hash": replay_preview["validation_hash"]}
        )
        assert replay["apply_status"] == "unchanged", replay
        assert sorted(replay["reused_client_ids"]) == sorted(applied["node_mappings"]), replay
        assert replay["target_compile_count"] == 0 and replay["saved"] is False, replay
    finally:
        await run.cleanup()


@pytest.mark.anyio
async def test_toolkit_facade_negatives_are_refused_live(mcp_client):
    """The four facade negatives in the fixtures are refused with their documented codes."""
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        host = await build_host_and_model(run)
        fixture = await build_adapter(run, host)
        envelope_only = envelope(
            fixture["adapter_object"],
            str(uuid.uuid4()),
            (await run.context(fixture["adapter_package"]))["fingerprint"],
            **fixture["intent"],
        )

        expected = {}
        for name in (
            "update-without-patch",
            "mixed-legacy-update-fields-with-patch",
            "coerced-boolean-flag",
            "unsupported-migration-op",
        ):
            expected[name] = load_fixture(f"negative/{name}.json")["expected"]["code"]

        # update-without-patch: the facade refuses instead of routing to the removed legacy batch.
        payload = await call(
            mcp_client,
            "blueprint_compose",
            {"mode": "update", "asset_path": fixture["adapter_object"], "nodes": [{"name": "LegacyNode", "class": "CallFunction"}]},
        )
        assert payload["_error"] == expected["update-without-patch"], payload

        # mixed-legacy-update-fields-with-patch: legacy fields together with a patch.
        payload = await call(
            mcp_client,
            "blueprint_compose",
            {
                "mode": "update",
                "asset_path": fixture["adapter_object"],
                "graph_name": "EventGraph",
                "connections": [{"from": "A.then", "to": "B.execute"}],
                "patch": envelope_only,
            },
        )
        assert payload["_error"] == expected["mixed-legacy-update-fields-with-patch"], payload

        # coerced-boolean-flag: flag types are never coerced.
        payload = await call(
            mcp_client,
            "blueprint_compose",
            {
                "mode": "update",
                "asset_path": fixture["adapter_object"],
                "patch": {**envelope_only, "dry_run": 1, "save": "false"},
            },
        )
        assert payload["_error"] == expected["coerced-boolean-flag"], payload

        # unsupported-migration-op: the native planner publishes only the four operations.
        payload = await graph_raw_apply(
            mcp_client,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                target={"graph_ref": {"graph_guid": fixture["ubergraph"]["graph_guid"]}},
                migration={"op": "relocate_nodes", "source": {"graph_ref": {"graph_guid": fixture["ubergraph"]["graph_guid"]}, "node_guids": []}},
            ),
        )
        assert payload["_error"] == expected["unsupported-migration-op"], payload
        assert "Unsupported migration operation" in payload["_message"], payload
    finally:
        await run.cleanup()


async def graph_raw_apply(client, body: dict) -> dict:
    return await call(client, "graph_cmd", {"command": "apply_patch", "params": body})


@pytest.mark.anyio
async def test_toolkit_live_only_negatives_are_refused_live(mcp_client):
    """The reachable `live_only` negatives are refused with the codes their descriptors declare.

    `expanded-boundary-pin` is declared rather than asserted: see `UNPRODUCIBLE` above.
    """
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        host = await build_host_and_model(run)
        fixture = await build_adapter(run, host)
        fingerprint = (await run.context(fixture["adapter_package"]))["fingerprint"]

        # stale-validation-token: a token from a preview of an earlier state is refused, and the
        # refusal happens before any mutation.
        stale_descriptor = load_fixture("negative/stale-validation-token.json")
        assert stale_descriptor["expected"]["code"] == "STALE_PRECONDITION"
        print_text = stale_descriptor["request"]["nodes"][0]
        earlier = await graph_raw_apply(
            mcp_client,
            envelope(fixture["adapter_object"], str(uuid.uuid4()), fingerprint, nodes=[print_text]),
        )
        earlier_token = earlier["validation_hash"]
        mutation = await graph_raw_apply(
            mcp_client,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                target={"graph_ref": {"graph_guid": fixture["ubergraph"]["graph_guid"]}},
                nodes=[
                    {
                        "client_id": "spare_print",
                        "node_class": "CallFunction",
                        "params": {"function_name": "KismetSystemLibrary.PrintString"},
                    }
                ],
            ),
        )
        applied = await graph_raw_apply(
            mcp_client,
            {**mutation, "dry_run": False, "expected_validation_hash": mutation["validation_hash"]},
        )
        assert applied["apply_status"] == "applied", applied
        before = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        stale = await graph_raw_apply(
            mcp_client,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                nodes=[print_text],
                dry_run=False,
                validation_hash=earlier_token,
            ),
        )
        assert stale["_error"] == "STALE_PRECONDITION", stale
        assert "expected_validation_hash does not match current preflight intent" in stale["_message"], stale
        after = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        assert after["node_count"] == before["node_count"], "a stale-token refusal mutated the graph"

        # parent-call-replace-entry: replace_entry never authors a parent call.
        parent_descriptor = load_fixture("negative/parent-call-replace-entry.json")
        assert parent_descriptor["expected"]["code"] == "INVALID_FIELD"
        entry_node_guid = applied["locators"]["entry_node_guid"]
        replace_entry = {
            "op": "replace_entry",
            "source": {
                "graph_ref": {"graph_guid": fixture["ubergraph"]["graph_guid"]},
                "entry_node_guid": entry_node_guid,
            },
            "pin_map": [
                {"entry": "output", "from_pin": "then", "to_pin": "then"},
                {"entry": "output", "from_pin": "Title", "to_pin": "Title"},
            ],
            "remove_shadowing_member": False,
        }
        payload = await graph_raw_apply(
            mcp_client,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                target={
                    "implementation": {
                        "owner_class": "/Script/CortexSandbox.CortexGraphAuthoringWidgetBase",
                        "function_name": "PresentTitle",
                        "call_kind": "parent",
                    }
                },
                migration=replace_entry,
            ),
        )
        assert payload["_error"] == parent_descriptor["expected"]["code"], payload
        assert "replace_entry preserves the body and never authors a parent call" in payload["_message"], payload

        # cross-graph-duplicate-identity: the planned destination identity already owned by another
        # graph is refused. The descriptor's code is asserted; the message is reported as observed.
        duplicate_descriptor = load_fixture("negative/cross-graph-duplicate-identity.json")
        assert duplicate_descriptor["expected"]["code"] == "INVALID_OPERATION"
        graphs = await run.graph_choices(fixture["adapter_package"])
        function_graphs = [choice for choice in graphs.values() if choice["graph_kind"] == "function"]
        ubergraph_nodes = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        selected = next(
            node for node in ubergraph_nodes["nodes"] if node["class"] == "K2Node_CallFunction"
        )
        if len(function_graphs) >= 2:
            transfer_patch_id = str(uuid.uuid4())
            source_ref = {"graph_guid": fixture["ubergraph"]["graph_guid"]}
            selection = {"graph_ref": source_ref, "node_guids": [selected["node_guid"]]}
            first = await graph_raw_apply(
                mcp_client,
                envelope(
                    fixture["adapter_object"],
                    transfer_patch_id,
                    (await run.context(fixture["adapter_package"]))["fingerprint"],
                    migration={
                        "op": "copy_subgraph",
                        "source": selection,
                        "destination": {"graph_ref": {"graph_guid": function_graphs[0]["graph_guid"]}},
                        "boundary": [],
                    },
                ),
            )
            second = await graph_raw_apply(
                mcp_client,
                envelope(
                    fixture["adapter_object"],
                    transfer_patch_id,
                    (await run.context(fixture["adapter_package"]))["fingerprint"],
                    migration={
                        "op": "copy_subgraph",
                        "source": selection,
                        "destination": {"graph_ref": {"graph_guid": function_graphs[1]["graph_guid"]}},
                        "boundary": [],
                    },
                ),
            )
            assert second["_error"] == duplicate_descriptor["expected"]["code"], second
            assert duplicate_descriptor["expected"]["message_contains"] in second["_message"], (
                first,
                second,
            )
    finally:
        await run.cleanup()
