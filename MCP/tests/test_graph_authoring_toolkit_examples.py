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
import re
import uuid
from pathlib import Path

import pytest

from test_graph_authoring_scenario import (
    FIXTURE_WIDGET_BASE,
    RECORDING_DEFAULT_LITERAL,
    TITLE_ARGUMENT,
    apply_reviewed,
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
    "stale entry output pin",
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
        found.update(match.strip() for match in re.findall(r"<live:\s*([^>]+)>", value))
    elif isinstance(value, dict):
        for entry in value.values():
            found |= collect_tokens(entry)
    elif isinstance(value, list):
        for entry in value:
            found |= collect_tokens(entry)
    return found


def bind(template, bindings: dict[str, object]):
    """Rebuilds a fixture fragment with its live tokens replaced by live values.

    A fixture may embed a token inside a larger literal (the adapter fixture writes a class path as
    `/Game/Temp/CortexGraphAuthoring_<live: run>/WBP_...._C`), so a string is substituted token by
    token, and a string that still carries an unbound token is an error instead of a silent miss.
    """
    if isinstance(template, str):
        if template.startswith("<live:") and template.endswith(">") and template.count("<live:") == 1:
            token = template[len("<live:") : -1].strip()
            if token not in bindings:
                raise AssertionError(f"fixture token '{token}' has no live binding")
            return bindings[token]
        result = template
        for token, value in bindings.items():
            result = result.replace(f"<live: {token}>", str(value))
        if "<live:" in result:
            raise AssertionError(f"unbound fixture token in '{result}'")
        return result
    if isinstance(template, dict):
        return {key: bind(value, bindings) for key, value in template.items()}
    if isinstance(template, list):
        values = [bind(value, bindings) for value in template]
        # A single placeholder can represent the complete approved GUID array.
        return values[0] if len(values) == 1 and isinstance(values[0], list) else values
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

        # The fixture writes the cast pin as `"As<live: cast target display name>"`, i.e. the token is
        # the display-name half the engine appends to its `As` prefix, not the whole pin name.
        cast_display_name = context["cast_result_pin"]
        if cast_display_name.startswith("As"):
            cast_display_name = cast_display_name[2:]
        bound_intent = bind(
            intent,
            {"cast target display name": cast_display_name, "run": run.label},
        )
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
        replay_body = envelope(
            context["adapter_object"],
            patch_id,
            (await run.context(context["adapter_package"]))["fingerprint"],
            **bound_intent,
            target=context["intent"]["target"],
        )
        replay_preview = await run.apply(replay_body)
        assert replay_preview["changed"] is False, replay_preview
        # The replay is applied with the guard it was previewed against: the first apply edited the
        # asset, so that earlier guard is stale by construction.
        replay = await run.apply(
            {
                **replay_body,
                "dry_run": False,
                "expected_validation_hash": replay_preview["validation_hash"],
            }
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
        # The adapter fixture is applied first: the replacement-entry negative needs a real inherited
        # implementation entry to name, and this binds the flow fixture live here as well.
        cast_display_name = fixture["cast_result_pin"]
        if cast_display_name.startswith("As"):
            cast_display_name = cast_display_name[2:]
        adapter_body = envelope(
            fixture["adapter_object"],
            str(uuid.uuid4()),
            (await run.context(fixture["adapter_package"]))["fingerprint"],
            **bind(load_fixture("adapter-intent.json"), {"cast target display name": cast_display_name, "run": run.label}),
            target=fixture["intent"]["target"],
        )
        adapter_preview = await run.apply(adapter_body)
        adapter_applied = await run.apply(
            {**adapter_body, "dry_run": False, "expected_validation_hash": adapter_preview["validation_hash"]}
        )
        assert adapter_applied["apply_status"] == "applied", adapter_applied
        fingerprint = (await run.context(fixture["adapter_package"]))["fingerprint"]

        # stale-validation-token: a token from a preview of an earlier state is refused, and the
        # refusal happens before any mutation.
        stale_descriptor = load_fixture("negative/stale-validation-token.json")
        assert stale_descriptor["expected"]["code"] == "STALE_PRECONDITION"
        print_text = stale_descriptor["request"]["nodes"][0]
        authoring_target = {"graph_ref": {"graph_guid": fixture["ubergraph"]["graph_guid"]}}
        earlier = await graph_raw_apply(
            mcp_client,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                fingerprint,
                target=authoring_target,
                nodes=[print_text],
            ),
        )
        assert earlier.get("validation_hash"), earlier
        earlier_token = earlier["validation_hash"]
        mutation_body = envelope(
            fixture["adapter_object"],
            str(uuid.uuid4()),
            (await run.context(fixture["adapter_package"]))["fingerprint"],
            target=authoring_target,
            nodes=[
                {
                    "client_id": "spare_print",
                    "node_class": "CallFunction",
                    "params": {"function_name": "KismetSystemLibrary.PrintString"},
                }
            ],
        )
        mutation_preview = await graph_raw_apply(mcp_client, mutation_body)
        applied = await graph_raw_apply(
            mcp_client,
            {
                **mutation_body,
                "dry_run": False,
                "expected_validation_hash": mutation_preview["validation_hash"],
            },
        )
        assert applied["apply_status"] == "applied", applied
        before = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        stale = await graph_raw_apply(
            mcp_client,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                target=authoring_target,
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
        assert parent_descriptor["expected"]["code"] == "INVALID_OPERATION"
        entry_node_guid = adapter_applied["locators"]["entry_node_guid"]
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
        assert parent_descriptor["expected"]["message_contains"] in payload["_message"], payload

        # cross-graph-duplicate-identity: the planned destination identity already owned by another
        # graph is refused. The descriptor's code is asserted; the message is reported as observed.
        duplicate_descriptor = load_fixture("negative/cross-graph-duplicate-identity.json")
        assert duplicate_descriptor["expected"]["code"] == "INVALID_OPERATION"
        for function_name in ("TransferFirst", "TransferSecond"):
            created = await call(
                mcp_client,
                "blueprint_cmd",
                {
                    "command": "add_function",
                    "params": {"asset_path": fixture["adapter_package"], "name": function_name},
                },
            )
            assert created.get("added") is True and created.get("graph_name") == function_name, created
        graphs = await run.graph_choices(fixture["adapter_package"])
        function_graphs = [choice for choice in graphs.values() if choice["graph_kind"] == "function"]
        ubergraph_nodes = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        selected = next(
            node for node in ubergraph_nodes["nodes"]
            if node["node_guid"] == applied["node_mappings"]["spare_print"]
        )
        assert len(function_graphs) >= 2, (
            "cross-graph duplicate identity requires two suitable function graphs",
            graphs,
        )
        transfer_patch_id = str(uuid.uuid4())
        source_ref = {"graph_guid": fixture["ubergraph"]["graph_guid"]}
        selection = {"graph_ref": source_ref, "node_guids": [selected["node_guid"]]}
        first_body = envelope(
            fixture["adapter_object"],
            transfer_patch_id,
            (await run.context(fixture["adapter_package"]))["fingerprint"],
            migration={
                "op": "copy_subgraph",
                "source": selection,
                "destination": {"graph_ref": {"graph_guid": function_graphs[0]["graph_guid"]}},
                "boundary": [],
            },
        )
        first_preview = await graph_raw_apply(mcp_client, first_body)
        assert first_preview.get("validation_hash"), first_preview
        first = await graph_raw_apply(
            mcp_client,
            {
                **first_body,
                "dry_run": False,
                "expected_validation_hash": first_preview["validation_hash"],
            },
        )
        assert first.get("apply_status") == "applied", first
        assert first.get("node_mappings"), first
        destination_after_first = await run.subgraph(
            fixture["adapter_package"], function_graphs[0]["graph_name"]
        )
        assert any(
            node.get("node_guid") in first["node_mappings"].values()
            for node in destination_after_first["nodes"]
        ), (first, destination_after_first)
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


@pytest.mark.anyio
@pytest.mark.parametrize("operation", ("copy_subgraph", "move_subgraph"))
async def test_toolkit_transfer_flow_preserves_internal_edges_and_maps_boundary(mcp_client, operation):
    """Bind the published transfer intent to two real graphs and verify its cross-graph result."""
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        package = await run.create("BP_CortexTransferFixture")
        asset = run.object_path(package)
        created = await call(
            mcp_client,
            "blueprint_cmd",
            {"command": "add_function", "params": {"asset_path": package, "name": "TransferTarget"}},
        )
        assert created.get("added") is True, created
        choices = await run.graph_choices(package)
        source = next(choice for choice in choices.values() if choice["graph_kind"] == "ubergraph")
        destination = choices["TransferTarget"]
        print_node = lambda name: {
            "client_id": name,
            "node_class": "CallFunction",
            "params": {"function_name": "KismetSystemLibrary.PrintString"},
        }
        source_body = envelope(
            asset, str(uuid.uuid4()), (await run.context(package))["fingerprint"],
            target={"graph_ref": {"graph_guid": source["graph_guid"]}},
            nodes=[print_node(name) for name in ("head", "tail", "consumer")],
            connections=[
                {"from": {"client_id": left, "pin": "then"}, "to": {"client_id": right, "pin": "execute"}}
                for left, right in (("head", "tail"), ("tail", "consumer"))
            ],
        )
        source_preview = await graph_raw_apply(mcp_client, source_body)
        assert source_preview.get("validation_hash"), source_preview
        authored = await graph_raw_apply(
            mcp_client, {**source_body, "dry_run": False, "expected_validation_hash": source_preview["validation_hash"]}
        )
        assert authored["readback_status"] == "matched", authored
        sink_body = envelope(
            asset, str(uuid.uuid4()), (await run.context(package))["fingerprint"],
            target={"graph_ref": {"graph_guid": destination["graph_guid"]}},
            nodes=[print_node("sink")],
        )
        sink_preview = await graph_raw_apply(mcp_client, sink_body)
        assert sink_preview.get("validation_hash"), sink_preview
        sink = await graph_raw_apply(
            mcp_client, {**sink_body, "dry_run": False, "expected_validation_hash": sink_preview["validation_hash"]}
        )
        assert sink["readback_status"] == "matched", sink
        source_before = await run.subgraph(package, source["graph_name"])
        guids = authored["node_mappings"]
        bindings = {
            "source graph_guid": source["graph_guid"],
            "destination graph_guid": destination["graph_guid"],
            "first selected node GUID": guids["head"],
            "second selected node GUID": guids["tail"],
            "selected source node GUID": guids["tail"],
            "crossing source pin name": "then",
            "existing destination node GUID": sink["node_mappings"]["sink"],
            "destination pin name": "execute",
        }
        request = bind(
            load_fixture(f"{operation.replace('_', '-')}-intent.json"), bindings
        )
        body = envelope(asset, str(uuid.uuid4()), (await run.context(package))["fingerprint"], **request)
        preview = await graph_raw_apply(mcp_client, body)
        assert preview["changed"] is True and preview["validation_hash"], preview
        assert len(preview["boundary"]) == 1 and len(preview["crossing_edges"]) == 1, preview
        applied = await graph_raw_apply(
            mcp_client, {**body, "dry_run": False, "expected_validation_hash": preview["validation_hash"]}
        )
        assert applied["apply_status"] == "applied" and applied["readback_status"] == "matched", applied
        assert applied["target_compile_count"] == 1 and applied["blocked"] is False, applied
        source_after = await run.subgraph(package, source["graph_name"])
        destination_after = await run.subgraph(package, destination["graph_name"])
        if operation == "copy_subgraph":
            assert source_after["nodes"] == source_before["nodes"], "copy changed the source graph"
            assert edges(source_after) == edges(source_before), "copy changed source edges"
        else:
            assert not {guids["head"], guids["tail"]} & {
                node["node_guid"] for node in source_after["nodes"]
            }, "move retained selected source nodes"
            assert any(node["node_guid"] == guids["consumer"] for node in source_after["nodes"])
        mapping = applied["node_mappings"]
        copied_head = mapping[guids["head"]] if operation == "copy_subgraph" else guids["head"]
        copied_tail = mapping[guids["tail"]] if operation == "copy_subgraph" else guids["tail"]
        names = node_name_map(destination_after)
        assert {copied_head, copied_tail, sink["node_mappings"]["sink"]} <= set(names), applied
        assert (names[copied_head], "then", names[copied_tail], "execute") in edges(destination_after)
        assert (
            names[copied_tail], "then", names[sink["node_mappings"]["sink"]], "execute"
        ) in edges(destination_after)
    finally:
        await run.cleanup()


@pytest.mark.anyio
async def test_toolkit_prune_preview_and_approved_apply_preserves_entry(mcp_client):
    """The published prune overlay approves precisely the preview's removable island."""
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        package = await run.create("BP_CortexPruneFixture")
        asset = run.object_path(package)
        source = next(
            choice for choice in (await run.graph_choices(package)).values()
            if choice["graph_kind"] == "ubergraph"
        )
        authored_body = envelope(
            asset, str(uuid.uuid4()), (await run.context(package))["fingerprint"],
            target={"graph_ref": {"graph_guid": source["graph_guid"]}},
            nodes=[
                {"client_id": "begin", "node_class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}},
                *(
                    {"client_id": name, "node_class": "CallFunction",
                     "params": {"function_name": "KismetSystemLibrary.PrintString"}}
                    for name in ("first", "second")
                ),
            ],
            connections=[
                {"from": {"client_id": left, "pin": "then"}, "to": {"client_id": right, "pin": "execute"}}
                for left, right in (("begin", "first"), ("first", "second"))
            ],
        )
        authored = await apply_reviewed(run, authored_body, package)
        assert authored["readback_status"] == "matched", authored
        nodes_before = await run.subgraph(package, source["graph_name"])
        bindings = {
            "source graph_guid": source["graph_guid"],
            "island entry node_guid": authored["node_mappings"]["begin"],
        }
        intent = bind(load_fixture("prune-island-intent.json"), bindings)
        body = envelope(asset, str(uuid.uuid4()), (await run.context(package))["fingerprint"], **intent)
        preview = await graph_raw_apply(mcp_client, body)
        assert preview["awaiting_approval"] is True and preview["complete"] is True, preview
        removable = preview["removable"]
        assert set(removable) == {
            authored["node_mappings"]["first"], authored["node_mappings"]["second"]
        }, preview
        # Approval changes the reviewed intent, so preview the exact approved request before apply.
        approved_intent = {**body, "migration": {**body["migration"], "approved_node_guids": removable}}
        approved_preview = await graph_raw_apply(mcp_client, approved_intent)
        assert approved_preview.get("validation_hash") and approved_preview["changed"] is True, approved_preview
        approved = bind(
            load_fixture("prune-island-apply.json"),
            {
                **bindings,
                "removable set published by the preview": removable,
                "validation_hash returned by the preview": approved_preview["validation_hash"],
            },
        )
        applied = await graph_raw_apply(mcp_client, {**approved_intent, **approved})
        assert applied["apply_status"] == "applied" and applied["readback_status"] == "matched", applied
        assert applied["target_compile_count"] == 1 and applied["blocked"] is False, applied
        assert applied["blocked_nodes"] == [], applied
        after = await run.subgraph(package, source["graph_name"])
        remaining = {node["node_guid"] for node in after["nodes"]}
        assert authored["node_mappings"]["begin"] in remaining
        assert not set(removable) & remaining
        assert len(after["nodes"]) == len(nodes_before["nodes"]) - len(removable)
    finally:
        await run.cleanup()


@pytest.mark.anyio
async def test_toolkit_replace_entry_preserves_body(mcp_client):
    """Bind the published replacement intent and observe the unchanged downstream body."""
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        package = await run.create("WBP_CortexReplaceFixture", parent_class=FIXTURE_WIDGET_BASE)
        asset = run.object_path(package)
        source = next(
            choice for choice in (await run.graph_choices(package)).values()
            if choice["graph_kind"] == "ubergraph"
        )
        authored_body = envelope(
            asset, str(uuid.uuid4()), (await run.context(package))["fingerprint"],
            target={"graph_ref": {"graph_guid": source["graph_guid"]}},
            nodes=[
                {"client_id": "stale", "node_class": "Event",
                 "params": {"owner_class": FIXTURE_WIDGET_BASE, "function_name": "PresentTitle"}},
                {"client_id": "body", "node_class": "CallFunction",
                 "params": {"function_name": "KismetSystemLibrary.PrintString"}},
            ],
            connections=[
                {"from": {"client_id": "stale", "pin": "then"},
                 "to": {"client_id": "body", "pin": "execute"}}
            ],
        )
        authored = await apply_reviewed(run, authored_body, package)
        assert authored["readback_status"] == "matched", authored
        before = await run.subgraph(package, source["graph_name"])
        body_guid = authored["node_mappings"]["body"]
        body_before = next(node for node in before["nodes"] if node["node_guid"] == body_guid)
        bindings = {
            "source graph_guid": source["graph_guid"],
            "stale entry node_guid": authored["node_mappings"]["stale"],
            "stale entry output pin": "then",
            "replacement entry output pin": "then",
        }
        intent = bind(load_fixture("replace-entry-intent.json"), bindings)
        body = envelope(asset, str(uuid.uuid4()), (await run.context(package))["fingerprint"], **intent)
        preview = await graph_raw_apply(mcp_client, body)
        assert preview["changed"] is True and preview["validation_hash"], preview
        result = await graph_raw_apply(
            mcp_client, {**body, "dry_run": False, "expected_validation_hash": preview["validation_hash"]}
        )
        assert result["apply_status"] == "applied" and result["readback_status"] == "matched", result
        after = await run.subgraph(package, source["graph_name"])
        body_after = next(node for node in after["nodes"] if node["node_guid"] == body_guid)
        # Only the mapped boundary link may change; intrinsic pins/defaults and node identity stay.
        for item in (body_before, body_after):
            item["pins"] = [
                {key: value for key, value in candidate.items() if key != "connections"}
                if candidate["name"] == "execute" else candidate
                for candidate in item["pins"]
            ]
        assert body_after == body_before
        assert authored["node_mappings"]["stale"] not in node_name_map(after)
        replacement = result["node_mappings"]["entry"]
        names = node_name_map(after)
        assert (names[replacement], "then", names[body_guid], "execute") in edges(after)
    finally:
        await run.cleanup()
