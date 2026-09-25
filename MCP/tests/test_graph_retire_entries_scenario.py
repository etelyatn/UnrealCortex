"""Live UE 5.8 scenario for stale-entry retirement through the registered MCP routes (plan Task 7).

The scenario drives the *registered* facade entrypoints against a live CortexSandbox editor:
`blueprint_cmd` (create/compile/cleanup_migration/delete), `graph_cmd` (get_authoring_context,
get_subgraph, apply_patch) and `blueprint_compose(mode="update")`. No test here calls a native C++
method directly.

The Blueprint starts parented to the generic `UCortexGraphRetireLegacyWidget` fixture, where the
legacy overrides are genuine `UK2Node_Event` entries. It is compiled while still parented there, then
reparented to `UCortexGraphRetireTargetWidget` *without* compiling: in UE 5.8 the compile of the
target-parent Blueprint converts every still-stale inherited override (one whose member the target
parent does not declare) into an unsupported `UK2Node_CustomEvent`, so retirement must run while the
real overrides are still eligible. `cleanup_migration(compile=false)` performs exactly the reparent
the native fixture does (`ParentClass` assignment + `RefreshAllNodes`) with no target compile.

The retained override is a *target-compatible* `UK2Node_Event` (`OnRetainedEvent` on the target
parent): the graph contract refuses an event override whose member is not inherited by the current
parent, so it is authored after the reparent, while the Blueprint is already parented to the target.
A source-parent override for the same name would be re-pointed to the target by the retirement
patch's compile, which would move a preserved node inside the verified transaction and the readback
would (correctly) refuse the apply.

This scenario does not claim to repair an initially compiler-invalid Blueprint; native tests cover the
separate `BS_Error` recovery contract.

Declared storage: every asset this module creates lives under
`/Game/Temp/CortexGraphRetire_<run>/` and cleanup deletes exactly those declared assets.

Run (editor lease required):
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_graph_retire_entries_scenario.py -v
"""

from __future__ import annotations

import json
import uuid

import pytest

pytestmark = pytest.mark.scenario

LEGACY_PARENT = "/Script/CortexGraph.CortexGraphRetireLegacyWidget"
TARGET_PARENT = "/Script/CortexGraph.CortexGraphRetireTargetWidget"
RUN_ROOT_PREFIX = "/Game/Temp/CortexGraphRetire_"


# ---------------------------------------------------------------------------
# Transport helpers
# ---------------------------------------------------------------------------


async def call(client, tool: str, args: dict) -> dict:
    result = await client.call_tool(tool, args)
    return json.loads(result.content[0].text)


def ok(payload: dict, context: str) -> dict:
    assert payload.get("success", True) is not False, f"{context}: {payload}"
    return payload


async def graph(client, command: str, params: dict | None = None) -> dict:
    payload = await call(client, "graph_cmd", {"command": command, "params": params or {}})
    return ok(payload, f"graph.{command}")


async def graph_raw(client, command: str, params: dict | None = None) -> dict:
    return await call(client, "graph_cmd", {"command": command, "params": params or {}})


async def blueprint(client, command: str, params: dict) -> dict:
    payload = await call(client, "blueprint_cmd", {"command": command, "params": params})
    return ok(payload, f"blueprint.{command}")


async def submit_patch(client, surface: str, envelope: dict) -> dict:
    """Send one reviewed patch envelope through the requested registered surface."""
    if surface == "graph_cmd":
        return await graph_raw(client, "apply_patch", envelope)
    assert surface == "blueprint_compose", surface
    patch = {key: value for key, value in envelope.items() if key != "asset_path"}
    return await call(
        client,
        "blueprint_compose",
        {"mode": "update", "asset_path": envelope["asset_path"], "patch": patch},
    )


# ---------------------------------------------------------------------------
# Declared run root
# ---------------------------------------------------------------------------


class RetireRun:
    """One declared asset root: every created asset is recorded and only those are deleted."""

    def __init__(self, client, label: str, surface: str):
        self.client = client
        self.label = label
        self.surface = surface
        self.root = f"{RUN_ROOT_PREFIX}{label}"
        self.package: str | None = None

    async def create(self) -> str:
        name = f"WBP_Retire_{self.label}"
        data = await blueprint(
            self.client,
            "create",
            {"name": name, "path": self.root, "type": "Widget", "parent_class": LEGACY_PARENT},
        )
        self.package = data["asset_path"]
        return self.package

    async def cleanup(self) -> None:
        if self.package is None:
            return
        package = self.package
        self.package = None
        try:
            payload = await call(
                self.client,
                "blueprint_cmd",
                {"command": "delete", "params": {"asset_path": package, "force": True}},
            )
        except Exception as exc:  # noqa: BLE001 - surfaced below, never swallowed
            raise AssertionError(f"cleanup failed for declared asset {package}: {exc!r}") from exc
        assert payload.get("success", True) is not False, f"cleanup failed for {package}: {payload}"

    @staticmethod
    def object_path(package: str) -> str:
        return f"{package}.{package.rsplit('/', 1)[-1]}"

    async def context(self, package: str) -> dict:
        return await graph(self.client, "get_authoring_context", {"asset_path": package})

    async def ubergraph(self, package: str) -> dict:
        data = await self.context(package)
        for choice in data["graph_choices"]:
            if choice.get("graph_kind") == "ubergraph":
                return choice
        raise AssertionError(f"no ubergraph in graph choices: {data['graph_choices']}")

    async def subgraph(self, package: str, graph_name: str) -> dict:
        return await graph(
            self.client,
            "get_subgraph",
            {"asset_path": package, "graph_name": graph_name, "include_edges": True},
        )

    async def fingerprint(self, package: str) -> dict:
        return (await self.context(package))["fingerprint"]

    async def author(self, package: str, graph_guid: str, nodes: list[dict], connections: list[dict]) -> dict:
        """One reviewed authoring patch through the run's surface; returns the applied result."""
        envelope = {
            "asset_path": self.object_path(package),
            "patch_id": str(uuid.uuid4()),
            "target": {"graph_ref": {"graph_guid": graph_guid}},
            "nodes": nodes,
            "connections": connections,
            "pin_updates": [],
            "compile": False,
            "save": False,
            "allow_noop": False,
        }
        applied: dict = {}
        for _ in range(2):
            attempt = {**envelope, "expected_fingerprint": await self.fingerprint(package)}
            preview = await submit_patch(self.client, self.surface, {**attempt, "dry_run": True})
            assert preview.get("validation_hash"), f"authoring preview: {preview}"
            applied = await submit_patch(
                self.client,
                self.surface,
                {**attempt, "dry_run": False, "expected_validation_hash": preview["validation_hash"]},
            )
            if applied.get("_error") != "STALE_PRECONDITION":
                return applied
        return applied


# ---------------------------------------------------------------------------
# Envelopes and readback helpers
# ---------------------------------------------------------------------------


def retire_envelope(
    object_path: str,
    patch_id: str,
    fingerprint: dict,
    graph_guid: str,
    entry_guids: list[str],
    *,
    dry_run: bool,
    approved: list[str] | None = None,
    validation_hash: str | None = None,
    compile: bool,
) -> dict:
    migration: dict = {
        "op": "retire_entries",
        "source": {
            "graph_ref": {"graph_guid": graph_guid},
            "entry_node_guids": list(entry_guids),
        },
    }
    if approved is not None:
        migration["approved_node_guids"] = list(approved)
    payload: dict = {
        "asset_path": object_path,
        "patch_id": patch_id,
        "expected_fingerprint": fingerprint,
        "migration": migration,
        "nodes": [],
        "connections": [],
        "pin_updates": [],
        "dry_run": dry_run,
        "compile": compile,
        "save": False,
        "allow_noop": False,
    }
    if validation_hash is not None:
        payload["expected_validation_hash"] = validation_hash
    return payload


def listed(values) -> list[str]:
    return sorted(str(value).lower() for value in values or [])


def node_class_map(subgraph: dict) -> dict[str, str]:
    return {node["node_guid"]: node["class"] for node in subgraph["nodes"] if node.get("node_guid")}


def edge_set(subgraph: dict) -> set[tuple[str, str, str, str]]:
    return {
        (edge["source_node"], edge["source_pin"], edge["target_node"], edge["target_pin"])
        for edge in subgraph.get("edges", [])
    }


def node_id(subgraph: dict, node_guid: str) -> str:
    for node in subgraph["nodes"]:
        if node.get("node_guid") == node_guid:
            return node["node_id"]
    raise AssertionError(f"node {node_guid} is absent from the graph")


# ---------------------------------------------------------------------------
# Graph construction
# ---------------------------------------------------------------------------


def legacy_source_spec() -> tuple[list[dict], list[dict]]:
    """The legacy overrides plus the body each removed entry owns, all created under the legacy
    parent. `producer` is shared with the retained body authored after the reparent."""
    nodes = [
        {"client_id": "alpha", "node_class": "Event", "params": {"function_name": "CortexGraphRetireLegacyWidget.OnLegacyAlpha"}},
        {"client_id": "beta", "node_class": "Event", "params": {"function_name": "CortexGraphRetireLegacyWidget.OnLegacyBeta"}},
        {"client_id": "producer", "node_class": "CallFunction", "params": {"function_name": "KismetStringLibrary.Conv_IntToString"}},
        {"client_id": "alpha_body", "node_class": "CallFunction", "params": {"function_name": "KismetSystemLibrary.PrintString"}},
        {"client_id": "beta_body", "node_class": "CallFunction", "params": {"function_name": "KismetSystemLibrary.PrintString"}},
    ]
    connections = [
        {"from": {"client_id": "alpha", "pin": "then"}, "to": {"client_id": "alpha_body", "pin": "execute"}},
        {"from": {"client_id": "beta", "pin": "then"}, "to": {"client_id": "beta_body", "pin": "execute"}},
        {"from": {"client_id": "producer", "pin": "ReturnValue"}, "to": {"client_id": "alpha_body", "pin": "InString"}},
        {"from": {"client_id": "producer", "pin": "ReturnValue"}, "to": {"client_id": "beta_body", "pin": "InString"}},
    ]
    return nodes, connections


def retained_target_spec(producer_guid: str) -> tuple[list[dict], list[dict]]:
    """The target-compatible retained override and its body, authored under the target parent. The
    shared producer is addressed by its node GUID so the retained link keeps using it."""
    nodes = [
        {"client_id": "retained", "node_class": "Event", "params": {"function_name": "CortexGraphRetireTargetWidget.OnRetainedEvent"}},
        {"client_id": "retained_body", "node_class": "CallFunction", "params": {"function_name": "KismetSystemLibrary.PrintString"}},
    ]
    connections = [
        {"from": {"client_id": "retained", "pin": "then"}, "to": {"client_id": "retained_body", "pin": "execute"}},
        {"from": {"node_guid": producer_guid, "pin": "ReturnValue"}, "to": {"client_id": "retained_body", "pin": "InString"}},
    ]
    return nodes, connections


# ---------------------------------------------------------------------------
# The live scenario
# ---------------------------------------------------------------------------


@pytest.mark.anyio
@pytest.mark.parametrize("surface", ["graph_cmd", "blueprint_compose"])
async def test_scenario_retire_entries_live_workflow(mcp_client, surface):
    run = RetireRun(mcp_client, uuid.uuid4().hex[:8], surface)
    body_error: BaseException | None = None
    try:
        package = await run.create()
        object_path = run.object_path(package)
        graph_guid = (await run.ubergraph(package))["graph_guid"]

        # 1. Genuine legacy overrides plus the bodies they own, created through the real graph tool
        #    while the Blueprint is still parented to the legacy fixture.
        nodes, connections = legacy_source_spec()
        built = await run.author(package, graph_guid, nodes, connections)
        assert built.get("success", True) is not False and built.get("apply_status") == "applied", built
        mappings = built["node_mappings"]
        alpha, beta = mappings["alpha"], mappings["beta"]
        producer, alpha_body, beta_body = mappings["producer"], mappings["alpha_body"], mappings["beta_body"]

        initial = await run.subgraph(package, (await run.ubergraph(package))["graph_name"])
        classes = node_class_map(initial)
        assert classes[alpha] == "K2Node_Event", classes
        assert classes[beta] == "K2Node_Event", classes

        # 2. Compile while still parented to the legacy fixture: the source Blueprint is valid.
        compiled = await blueprint(mcp_client, "compile", {"asset_path": package})
        assert compiled["error_count"] == 0, compiled
        assert compiled["compile_status"] == "success", compiled

        # 3. Reparent to the target *without* any target-parent compile. The registered `reparent`
        #    route compiles internally, which is exactly what UE 5.8 normalizes the stale overrides
        #    with, so the no-compile cleanup migration performs the same reparent the native fixture
        #    does (direct ParentClass assignment + RefreshAllNodes).
        reparented = await blueprint(
            mcp_client,
            "cleanup_migration",
            {"asset_path": package, "new_parent_class": TARGET_PARENT, "compile": False},
        )
        assert reparented["reparented"] is True, reparented
        assert reparented["compiled"] is False, reparented
        assert reparented["new_parent"] == "CortexGraphRetireTargetWidget", reparented

        # 4. The stale overrides are still genuine `UK2Node_Event` overrides: no compile has run on
        #    the target-parent Blueprint yet, so retirement still has eligible entries to remove.
        reparented_classes = node_class_map(await run.subgraph(package, (await run.ubergraph(package))["graph_name"]))
        assert reparented_classes[alpha] == "K2Node_Event", reparented_classes
        assert reparented_classes[beta] == "K2Node_Event", reparented_classes

        # 5. Author the target-compatible retained override and its body under the target parent (no
        #    compile), reusing the shared producer by GUID for the retained data link.
        retained_spec = retained_target_spec(producer)
        retained_result = await run.author(package, graph_guid, *retained_spec)
        assert retained_result.get("apply_status") == "applied", retained_result
        retained = retained_result["node_mappings"]["retained"]
        retained_body = retained_result["node_mappings"]["retained_body"]
        retained_classes = node_class_map(await run.subgraph(package, (await run.ubergraph(package))["graph_name"]))
        assert retained_classes[retained] == "K2Node_Event", retained_classes

        # 6. Fresh fingerprint, complete preview of the exact retirement set, then review and approve
        #    that exact set. The approval token is bound to the patch identity and the reviewed
        #    approval set, so the reviewed preview and its apply share one patch id and one set.
        patch_id = str(uuid.uuid4())
        for _ in range(2):
            fingerprint = await run.fingerprint(package)
            preview = await submit_patch(
                mcp_client,
                surface,
                retire_envelope(object_path, patch_id, fingerprint, graph_guid, [alpha, beta],
                                dry_run=True, compile=False),
            )
            assert preview.get("success", True) is not False, preview
            assert preview.get("complete") is True, preview
            removable = list(preview["removable"])
            assert alpha in removable and beta in removable, preview
            for kept in (retained, retained_body, producer):
                assert kept not in removable, f"retained node {kept} is in the removable set: {preview}"
            assert preview.get("approved_guids") == [], preview

            reviewed = await submit_patch(
                mcp_client,
                surface,
                retire_envelope(object_path, patch_id, fingerprint, graph_guid, [alpha, beta],
                                dry_run=True, approved=removable, compile=False),
            )
            assert reviewed.get("success", True) is not False, reviewed
            assert listed(reviewed.get("approved_guids")) == listed(removable), reviewed

            applied = await submit_patch(
                mcp_client,
                surface,
                retire_envelope(object_path, patch_id, fingerprint, graph_guid, [alpha, beta],
                                dry_run=False, approved=removable,
                                validation_hash=reviewed["validation_hash"], compile=True),
            )
            if applied.get("_error") != "STALE_PRECONDITION":
                break

        # 7. The apply removes the approved set and its one post-removal target compile validates the
        #    final graph: applied, compiled, matched readback, no save, no other compile.
        assert applied.get("success", True) is not False, applied
        assert applied["apply_status"] == "applied", applied
        assert applied["compile_status"] == "compiled", applied
        assert applied["readback_status"] == "matched", applied
        assert applied["target_compile_count"] == 1, applied
        assert applied["recovery_compile_count"] == 0, applied
        assert applied["saved"] is False, applied
        assert applied["blocked"] is False, applied

        # 8. Readback: the approved GUIDs are absent, the retained override keeps its body link and
        #    the shared producer link, and no second compile or save reached that state.
        final_graph = await run.subgraph(package, (await run.ubergraph(package))["graph_name"])
        final_guids = {node["node_guid"] for node in final_graph["nodes"]}
        assert alpha not in final_guids and beta not in final_guids, final_guids
        assert {retained, retained_body, producer} <= final_guids, final_guids
        assert {alpha_body, beta_body}.isdisjoint(final_guids), final_guids
        final_classes = node_class_map(final_graph)
        assert final_classes[retained] == "K2Node_Event", final_classes
        read_edges = edge_set(final_graph)
        assert (node_id(final_graph, retained), "then", node_id(final_graph, retained_body), "execute") in read_edges
        assert (
            node_id(final_graph, producer),
            "ReturnValue",
            node_id(final_graph, retained_body),
            "InString",
        ) in read_edges
    except BaseException as exc:
        body_error = exc
        raise
    finally:
        try:
            await run.cleanup()
        except Exception as cleanup_error:
            if body_error is not None:
                body_error.add_note(f"cleanup also failed: {cleanup_error}")
            else:
                raise
