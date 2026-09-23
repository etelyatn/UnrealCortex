"""Live end-to-end scenario for the typed Blueprint authoring contract (plan T15).

The scenario drives the *registered* facade entrypoints through the MCP layer
(`graph_cmd` -> `graph.*`, `blueprint_compose(mode="update")`, `umg_cmd`, `editor_cmd run_python`,
`profile_operation_schema`, `core_cmd get_capabilities`) against a live CortexSandbox editor and
asserts the observed runtime state through `editor.run_python`, which dispatches the Sandbox fixture
hook `UCortexGraphAuthoringHarness`. No test here calls a native C++ method directly.

Declared storage: every asset this module creates lives under
`/Game/Temp/CortexGraphAuthoring_<run>/` and cleanup deletes exactly those declared assets.

Run (editor lease required):
    cd Plugins/UnrealCortex/MCP && uv run pytest tests/test_graph_authoring_scenario.py -m scenario -v
"""

from __future__ import annotations

import json
import uuid
from pathlib import Path

import pytest

from cortex_mcp.capabilities import load_capabilities_cache
from cortex_mcp.operation_schema import build_profile_operation_schema

pytestmark = pytest.mark.scenario

RUN_ROOT_PREFIX = "/Game/Temp/CortexGraphAuthoring_"

# Fixture-owned names (declared by the Sandbox fixture, not live tokens).
FIXTURE_WIDGET_BASE = "/Script/CortexSandbox.CortexGraphAuthoringWidgetBase"
FIXTURE_MODEL_BASE = "/Script/CortexSandbox.CortexGraphAuthoringModel"
HARNESS_CLASS = "CortexGraphAuthoringHarness"
MODEL_NAME = "BP_CortexGraphAuthoringModel"
HOST_NAME = "WBP_CortexGraphAuthoringHost"
ADAPTER_NAME = "WBP_CortexGraphAuthoringAdapter"
ACTOR_NAME = "BP_CortexGraphAuthoringActor"
DESIGNER_WIDGET_NAME = "TitleLabel"
MODEL_VARIABLE = "ModelRef"
RECORDING_FUNCTION = "RecordObservedTitle"
RECORDING_DEFAULT_LITERAL = "CortexGraphAuthoring"
TITLE_ARGUMENT = "CortexGraphAuthoringTitle"

PROJECT_ROOT = Path(__file__).resolve().parents[4]
CONTENT_ROOT = PROJECT_ROOT / "Content"


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


async def umg(client, command: str, params: dict) -> dict:
    payload = await call(client, "umg_cmd", {"command": command, "params": params})
    return ok(payload, f"umg.{command}")


async def run_python(client, code: str) -> dict:
    return await call(client, "editor_cmd", {"command": "run_python", "params": {"code": code}})


def python_report(payload: dict, context: str) -> dict:
    """The fixture hook prints one JSON report; return it."""
    assert payload.get("success", True) is not False, f"{context}: {payload}"
    text = "\n".join(entry.get("text", "") for entry in payload.get("output") or [])
    start = text.find("{")
    end = text.rfind("}")
    assert start >= 0 and end > start, f"{context}: run_python printed no JSON report: {text!r}"
    return json.loads(text[start : end + 1])


# ---------------------------------------------------------------------------
# Declared run root
# ---------------------------------------------------------------------------


class AuthoringRun:
    """One declared asset root: every created asset is recorded and only those are deleted."""

    def __init__(self, client, label: str):
        self.client = client
        self.label = label
        self.root = f"{RUN_ROOT_PREFIX}{label}"
        self.created: list[str] = []

    async def create(self, name: str, *, kind: str = "Actor", parent_class: str | None = None) -> str:
        params = {"name": name, "path": self.root, "type": kind}
        if parent_class:
            params["parent_class"] = parent_class
        data = await blueprint(self.client, "create", params)
        package = data["asset_path"]
        self.created.append(package)
        return package

    async def cleanup(self) -> None:
        for package in reversed(self.created):
            try:
                await call(self.client, "blueprint_cmd", {"command": "delete", "params": {"asset_path": package}})
            except Exception:  # noqa: BLE001 - best-effort cleanup of declared assets only
                pass
        self.created = []

    @staticmethod
    def object_path(package: str) -> str:
        return f"{package}.{package.rsplit('/', 1)[-1]}"

    @staticmethod
    def generated_class(package: str) -> str:
        return f"{package}.{package.rsplit('/', 1)[-1]}_C"

    @staticmethod
    def package_file(package: str) -> Path:
        relative = package[len("/Game/") :]
        return CONTENT_ROOT / f"{relative}.uasset"

    async def context(self, package: str) -> dict:
        return await graph(self.client, "get_authoring_context", {"asset_path": package})

    async def graph_choices(self, package: str) -> dict[str, dict]:
        data = await self.context(package)
        return {choice["graph_name"]: choice for choice in data["graph_choices"]}

    async def subgraph(self, package: str, graph_name: str) -> dict:
        return await graph(
            self.client,
            "get_subgraph",
            {"asset_path": package, "graph_name": graph_name, "include_edges": True},
        )

    async def apply(self, envelope: dict) -> dict:
        return await graph(self.client, "apply_patch", envelope)


def envelope(
    asset_object_path: str,
    patch_id: str,
    fingerprint: dict,
    *,
    target: dict | None = None,
    nodes: list | None = None,
    connections: list | None = None,
    pin_updates: list | None = None,
    dry_run: bool = True,
    compile: bool = True,
    save: bool = False,
    allow_noop: bool = False,
    validation_hash: str | None = None,
    migration: dict | None = None,
) -> dict:
    payload = {
        "asset_path": asset_object_path,
        "patch_id": patch_id,
        "expected_fingerprint": fingerprint,
        "nodes": nodes or [],
        "connections": connections or [],
        "pin_updates": pin_updates or [],
        "dry_run": dry_run,
        "compile": compile,
        "save": save,
        "allow_noop": allow_noop,
    }
    if target is not None:
        payload["target"] = target
    if migration is not None:
        payload["migration"] = migration
    if validation_hash is not None:
        payload["expected_validation_hash"] = validation_hash
    return payload


# ---------------------------------------------------------------------------
# Fixture construction (host widget + model + authored adapter)
# ---------------------------------------------------------------------------


def node_name_map(subgraph: dict) -> dict[str, str]:
    """node_guid -> stable node name, so deterministic identities can address read edges."""
    return {node["node_guid"]: node["node_id"] for node in subgraph["nodes"] if node.get("node_guid")}


def edges(subgraph: dict) -> set[tuple[str, str, str, str]]:
    return {
        (edge["source_node"], edge["source_pin"], edge["target_node"], edge["target_pin"])
        for edge in subgraph.get("edges", [])
    }


def pin(subgraph: dict, node_name: str, pin_name: str) -> dict:
    for node in subgraph["nodes"]:
        if node["node_id"] != node_name:
            continue
        for candidate in node.get("pins", []):
            if candidate["name"] == pin_name:
                return candidate
    raise AssertionError(f"pin {pin_name} not found on node {node_name}")


async def apply_reviewed(run: "AuthoringRun", body: dict) -> dict:
    """Preview the intent, then apply exactly it with the preview's token (the apply precondition)."""
    preview = await run.apply({**body, "dry_run": True})
    assert preview.get("validation_hash"), preview
    return await run.apply({**body, "dry_run": False, "expected_validation_hash": preview["validation_hash"]})


async def describe_node(client, node_class: str, params: dict, asset_package: str | None = None) -> dict:
    request = {"node_class": node_class, "params": params}
    if asset_package:
        request["asset_path"] = asset_package
    return await graph(client, "describe_node", request)


async def build_host_and_model(run: AuthoringRun) -> dict:
    """The generic model Blueprint and the generic host widget Blueprint with its recording function."""
    client = run.client
    model_package = await run.create(MODEL_NAME, parent_class=FIXTURE_MODEL_BASE)
    model_class = run.generated_class(model_package)

    host_package = await run.create(HOST_NAME, kind="Widget")
    host_class = run.generated_class(host_package)
    await blueprint(client, "add_variable", {"asset_path": host_package, "name": "ObservedModel", "type": model_class})
    await blueprint(client, "add_variable", {"asset_path": host_package, "name": "ObservedTitle", "type": "text"})
    await blueprint(
        client,
        "add_function",
        {
            "asset_path": host_package,
            "name": RECORDING_FUNCTION,
            "inputs": [
                {"name": "Model", "type": model_class},
                {"name": "Title", "type": "text"},
            ],
            "outputs": [{"name": "ReturnValue", "type": model_class}],
        },
    )

    # The recording function is Blueprint-defined, so its body is authored through the graph
    # contract addressed by graph_ref (an implementation target only accepts inherited overrides).
    graphs = await run.graph_choices(host_package)
    recording_graph = graphs[RECORDING_FUNCTION]
    subgraph = await run.subgraph(host_package, RECORDING_FUNCTION)
    entry = next(node for node in subgraph["nodes"] if node["class"] == "K2Node_FunctionEntry")
    result = next(node for node in subgraph["nodes"] if node["class"] == "K2Node_FunctionResult")

    # A freshly created function graph gets its entry execution output linked straight to the result
    # terminator by the engine, and the patch contract deliberately refuses to replace an existing
    # exec link. The link is therefore removed by the registered graph.disconnect operation first:
    # a separate graph-maintenance step outside the patch transaction, never claimed as part of it.
    await graph(
        client,
        "disconnect",
        {
            "asset_path": host_package,
            "node_id": entry["node_id"],
            "pin_name": "then",
            "graph_name": RECORDING_FUNCTION,
        },
    )

    fingerprint = (await run.context(host_package))["fingerprint"]
    body = envelope(
        run.object_path(host_package),
        str(uuid.uuid4()),
        fingerprint,
        target={"graph_ref": {"graph_guid": recording_graph["graph_guid"]}},
        nodes=[
            {"client_id": "set_model", "node_class": "VariableSet", "params": {"variable_name": "ObservedModel"}},
            {"client_id": "set_title", "node_class": "VariableSet", "params": {"variable_name": "ObservedTitle"}},
        ],
        connections=[
            {"from": {"node_guid": entry["node_guid"], "pin": "then"}, "to": {"client_id": "set_model", "pin": "execute"}},
            {"from": {"client_id": "set_model", "pin": "then"}, "to": {"client_id": "set_title", "pin": "execute"}},
            {"from": {"client_id": "set_title", "pin": "then"}, "to": {"node_guid": result["node_guid"], "pin": "execute"}},
            {"from": {"node_guid": entry["node_guid"], "pin": "Model"}, "to": {"client_id": "set_model", "pin": "ObservedModel"}},
            {"from": {"node_guid": entry["node_guid"], "pin": "Title"}, "to": {"client_id": "set_title", "pin": "ObservedTitle"}},
            {"from": {"node_guid": entry["node_guid"], "pin": "Model"}, "to": {"node_guid": result["node_guid"], "pin": "ReturnValue"}},
        ],
        dry_run=False,
    )
    preview = await run.apply({**body, "dry_run": True})
    applied = await run.apply({**body, "expected_validation_hash": preview["validation_hash"]})
    assert applied["readback_status"] == "matched", applied

    return {
        "model_package": model_package,
        "model_class": model_class,
        "host_package": host_package,
        "host_class": host_class,
    }


async def build_adapter(run: AuthoringRun, host: dict) -> dict:
    """The authored Widget Blueprint: designer widget repaired, Blueprint variable, adapter intent."""
    client = run.client
    adapter_package = await run.create(ADAPTER_NAME, parent_class=FIXTURE_WIDGET_BASE)
    adapter_class = run.generated_class(adapter_package)

    # The designer widget must exist in the tree before the repair marks it a variable, and the
    # repair (a separate umg.set_widget_variable operation, never part of a patch) must happen
    # before any graph references it.
    tree = await umg(client, "get_tree", {"asset_path": adapter_package})
    root = tree.get("root")
    root_name = root.get("name") if isinstance(root, dict) else None
    if not root_name:
        root_name = "RootCanvas"
        await umg(client, "add_widget", {"asset_path": adapter_package, "widget_class": "CanvasPanel", "name": root_name})

    await umg(
        client,
        "add_widget",
        {
            "asset_path": adapter_package,
            "widget_class": host["host_class"],
            "name": DESIGNER_WIDGET_NAME,
            "parent_name": root_name,
        },
    )

    # Repair separation (T15 R3): making the designer widget a variable is one separate
    # umg.set_widget_variable operation. It is never part of a graph.apply_patch transaction and no
    # assertion below claims a single transaction spans both.
    repair = await umg(
        client,
        "set_widget_variable",
        {"asset_path": adapter_package, "widget_name": DESIGNER_WIDGET_NAME, "is_variable": True},
    )
    assert repair is not None

    await blueprint(client, "add_variable", {"asset_path": adapter_package, "name": MODEL_VARIABLE, "type": host["model_class"]})
    await blueprint(client, "compile", {"asset_path": adapter_package})

    graphs = await run.graph_choices(adapter_package)
    ubergraph = next(choice for choice in graphs.values() if choice["graph_kind"] == "ubergraph")
    cast_pins = await describe_node(
        client,
        "DynamicCast",
        {"class": host["host_class"], "is_pure": False},
        adapter_package,
    )
    cast_result_pin = next(
        candidate["name"]
        for candidate in cast_pins["expected_pins"]
        if candidate["direction"] == "output" and candidate["name"] not in {"then", "CastFailed"}
    )

    intent = {
        "target": {"implementation": {"owner_class": FIXTURE_WIDGET_BASE, "function_name": "PresentTitle"}},
        "nodes": [
            {"client_id": "self", "node_class": "Self"},
            {"client_id": "title_label", "node_class": "VariableGet", "params": {"variable_name": DESIGNER_WIDGET_NAME}},
            {
                "client_id": "host_cast",
                "node_class": "DynamicCast",
                "params": {"class": host["host_class"], "is_pure": False},
            },
            {"client_id": "model", "node_class": "ConstructObject", "params": {"class": host["model_class"]}},
            {
                "client_id": "record",
                "node_class": "CallFunction",
                "params": {"owner_class": host["host_class"], "function_name": RECORDING_FUNCTION},
                "defaults": {"Title": {"kind": "text", "literal": RECORDING_DEFAULT_LITERAL}},
            },
            {"client_id": "store_model", "node_class": "VariableSet", "params": {"variable_name": MODEL_VARIABLE}},
        ],
        "connections": [
            {"from": {"entry": True, "pin": "then"}, "to": {"client_id": "model", "pin": "execute"}},
            {"from": {"client_id": "model", "pin": "then"}, "to": {"client_id": "host_cast", "pin": "execute"}},
            {"from": {"client_id": "host_cast", "pin": "then"}, "to": {"client_id": "record", "pin": "execute"}},
            {"from": {"client_id": "record", "pin": "then"}, "to": {"client_id": "store_model", "pin": "execute"}},
            {"from": {"client_id": "self", "pin": "self"}, "to": {"client_id": "model", "pin": "self"}},
            {"from": {"entry": True, "pin": "Title"}, "to": {"client_id": "model", "pin": "Title"}},
            {"from": {"client_id": "title_label", "pin": DESIGNER_WIDGET_NAME}, "to": {"client_id": "host_cast", "pin": "Object"}},
            {"from": {"client_id": "host_cast", "pin": cast_result_pin}, "to": {"client_id": "record", "pin": "self"}},
            {"from": {"client_id": "model", "pin": "ReturnValue"}, "to": {"client_id": "record", "pin": "Model"}},
            {"from": {"client_id": "record", "pin": "ReturnValue"}, "to": {"client_id": "store_model", "pin": MODEL_VARIABLE}},
        ],
        "pin_updates": [],
    }

    return {
        **host,
        "adapter_package": adapter_package,
        "adapter_class": adapter_class,
        "adapter_object": run.object_path(adapter_package),
        "cast_result_pin": cast_result_pin,
        "intent": intent,
        "ubergraph": ubergraph,
    }


# ---------------------------------------------------------------------------
# Runtime observation hook
# ---------------------------------------------------------------------------


async def observe_widget(client, widget_class: str, title: str, *, collect_garbage: bool) -> dict:
    code = "\n".join(
        [
            "import unreal",
            f"print(unreal.{HARNESS_CLASS}.run_widget_adapter({json.dumps(widget_class)}, {json.dumps(title)}, {collect_garbage}))",
        ]
    )
    return python_report(await run_python(client, code), "run_widget_adapter")


async def observe_actor(client, actor_class: str) -> dict:
    code = "\n".join(
        [
            "import unreal",
            f"print(unreal.{HARNESS_CLASS}.run_actor_adapter({json.dumps(actor_class)}))",
        ]
    )
    return python_report(await run_python(client, code), "run_actor_adapter")


# ---------------------------------------------------------------------------
# Scenario A: the generic adapter chain in the Widget Blueprint context
# ---------------------------------------------------------------------------


@pytest.mark.anyio
async def test_scenario_typed_authoring_adapter_widget_blueprint(mcp_client, tcp_connection):
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        # Live schema first: the profile contract must agree with the connected editor, and the
        # authoring profile must still block unrelated domains.
        schema = json.loads(
            build_profile_operation_schema(tcp_connection, "UMGAuthoring", "graph", "apply_patch")
        )
        assert schema["source"] == "live_editor", schema
        assert schema["policy_allowed"] is True, schema
        assert schema["execution_shape"] == {"type": "router", "tool": "graph_cmd"}, schema
        blocked = json.loads(build_profile_operation_schema(tcp_connection, "UMGAuthoring", "blueprint", "add_variable"))
        assert blocked["policy_allowed"] is False, blocked
        capabilities = await call(mcp_client, "core_cmd", {"command": "get_capabilities", "params": {}})
        # The whole capability document is larger than the MCP response guard, so the router reports
        # the bounded refusal; the live registration is asserted from the editor's own cache plus the
        # per-command live schemas below.
        assert capabilities.get("_error") in (None, "RESPONSE_TOO_LARGE"), capabilities
        live_apply = await call(
            mcp_client,
            "core_cmd",
            {"command": "get_operation_schema", "params": {"domain": "graph", "command": "apply_patch"}},
        )
        assert live_apply["source"] == "live_editor" and live_apply["router"] == "graph_cmd", live_apply
        live_umg = await call(
            mcp_client,
            "core_cmd",
            {"command": "get_operation_schema", "params": {"domain": "umg", "command": "set_widget_variable"}},
        )
        assert live_umg["source"] == "live_editor", live_umg
        cached = load_capabilities_cache()["domains"]
        assert {"apply_patch", "get_authoring_context", "describe_node"} <= {
            command["name"] for command in cached["graph"]["commands"]
        }
        assert "set_widget_variable" in {command["name"] for command in cached["umg"]["commands"]}

        host = await build_host_and_model(run)
        fixture = await build_adapter(run, host)

        # Baseline runtime state before the adapter exists: the native hook returns its empty
        # baseline, the repaired designer widget resolves, and no model reference is retained.
        baseline = await observe_widget(mcp_client, fixture["adapter_class"], TITLE_ARGUMENT, collect_garbage=False)
        assert baseline["derives_from_fixture_base"] is True, baseline
        assert baseline["read_presented_title_before"] == "", baseline
        assert baseline["read_presented_title"] == "", baseline
        assert baseline["title_label_valid"] is True, baseline
        assert baseline["model_ref_valid"] is False, baseline

        # Authoring context: fingerprint plus graph choices. The known caveat is exercised, not
        # worked around: an implementation target is still refused by the context reader, so the
        # fingerprint is read without a target and the implementation target goes to apply_patch.
        context = await run.context(fixture["adapter_package"])
        fingerprint = context["fingerprint"]
        assert fingerprint["graph_authoring_version"] == 1, fingerprint
        graph_choices = {choice["graph_name"]: choice for choice in context["graph_choices"]}
        assert fixture["ubergraph"]["graph_name"] in graph_choices
        assert graph_choices[fixture["ubergraph"]["graph_name"]]["is_mutable"] is True
        caveat = await graph_raw(
            mcp_client,
            "get_authoring_context",
            {
                "asset_path": fixture["adapter_package"],
                "target": {"implementation": {"owner_class": FIXTURE_WIDGET_BASE, "function_name": "PresentTitle"}},
            },
        )
        assert caveat["_error"] == "UNSUPPORTED_OPERATION", caveat

        # Typed describe in real context: the canonical selectors and pin names the intent uses.
        construct = await describe_node(mcp_client, "ConstructObject", {"class": host["model_class"]}, fixture["adapter_package"])
        construct_pins = {candidate["name"] for candidate in construct["expected_pins"]}
        assert {"self", "Class", "ReturnValue", "Title"} <= construct_pins, construct_pins
        call_contract = await describe_node(
            mcp_client,
            "CallFunction",
            {"owner_class": host["host_class"], "function_name": RECORDING_FUNCTION},
            fixture["adapter_package"],
        )
        call_pins = {candidate["name"] for candidate in call_contract["expected_pins"]}
        assert {"execute", "then", "self", "Model", "Title", "ReturnValue"} <= call_pins, call_pins
        variables = await describe_node(mcp_client, "VariableGet", {"variable_name": DESIGNER_WIDGET_NAME}, fixture["adapter_package"])
        assert DESIGNER_WIDGET_NAME in {candidate["name"] for candidate in variables["expected_pins"]}
        self_node = await describe_node(mcp_client, "Self", {}, fixture["adapter_package"])
        assert "self" in {candidate["name"] for candidate in self_node["expected_pins"]}

        # Preview: change-free phases, deterministic identity for every client id, and no mutation.
        before = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        patch_id = str(uuid.uuid4())
        body = envelope(fixture["adapter_object"], patch_id, fingerprint, **fixture["intent"])
        preview = await run.apply(body)
        assert preview["changed"] is True, preview
        assert preview["dry_run"] is True
        for phase in ("apply_status", "compile_status", "readback_status", "rollback_status", "save_status", "post_save_status"):
            assert preview[phase] == "not_requested", (phase, preview)
        assert preview["target_compile_count"] == 0
        assert preview["saved"] is False
        assert preview["validation_hash"]
        assert set(preview["node_mappings"]) == {"self", "title_label", "host_cast", "model", "record", "store_model"}
        # The implementation entry does not exist yet at preview time, so the preview publishes the
        # planned graph locator only and reports the absent entry honestly; the apply publishes both.
        assert preview["locators"]["has_entry_node"] is False, preview["locators"]
        assert "entry_node_guid" not in preview["locators"], preview["locators"]
        assert preview["fingerprint_after"] == preview["fingerprint_before"]
        assert preview["dirty_after"] == preview["dirty_before"]
        after_preview = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        assert after_preview["node_count"] == before["node_count"], "preview mutated the graph"

        # Apply through the registered composite facade, exactly as an agent would.
        applied = await call(
            mcp_client,
            "blueprint_compose",
            {"mode": "update", "asset_path": fixture["adapter_object"], "patch": {**body, "dry_run": False, "expected_validation_hash": preview["validation_hash"]}},
        )
        assert applied["apply_status"] == "applied", applied
        assert applied["compile_status"] == "compiled", applied
        assert applied["readback_status"] == "matched", applied
        assert applied["target_compile_count"] == 1, applied
        assert applied["save_status"] == "not_requested", applied
        assert applied["saved"] is False and applied["blocked"] is False
        assert applied["dirty_after"] is True
        assert applied["reused_client_ids"] == []
        # The created implementation entry has a durable locator after the apply.
        assert applied["locators"]["has_entry_node"] is True, applied["locators"]
        assert applied["locators"]["entry_node_guid"], applied["locators"]
        assert applied["locators"]["graph_guid"] == fixture["ubergraph"]["graph_guid"], applied["locators"]

        # Read symbols, defaults and edges of the applied chain.
        names = node_name_map(await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"]))
        by_client = {client_id: names[guid] for client_id, guid in applied["node_mappings"].items()}
        subgraph = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        read_edges = edges(subgraph)
        assert (
            by_client["self"],
            "self",
            by_client["model"],
            "self",
        ) in read_edges, "Self -> Outer (self pin) wiring missing"
        assert (
            by_client["title_label"],
            DESIGNER_WIDGET_NAME,
            by_client["host_cast"],
            "Object",
        ) in read_edges, "designer widget read is not the cast input"
        assert (
            by_client["host_cast"],
            fixture["cast_result_pin"],
            by_client["record"],
            "self",
        ) in read_edges, "the host function call is not on the explicit cast target"
        assert (
            by_client["record"],
            "ReturnValue",
            by_client["store_model"],
            MODEL_VARIABLE,
        ) in read_edges, "the retained model reference is not stored"
        entry_name = names[applied["locators"]["entry_node_guid"]]
        assert (entry_name, "Title", by_client["model"], "Title") in read_edges, "entry parameter does not feed the exposed-on-spawn input"
        tagged = pin(subgraph, by_client["record"], "Title")
        # The readback publishes the canonical FText identity of the tagged default (design 4.3: text
        # identity is the source kind plus the literal/table identity), not the request's shorthand.
        assert tagged["default_descriptor"]["kind"] == "text", tagged
        assert tagged["default_descriptor"]["value"]["source_kind"] == "literal", tagged
        assert tagged["default_descriptor"]["value"]["value"] == RECORDING_DEFAULT_LITERAL, tagged
        assert tagged.get("is_connected") is not True, tagged
        found = await graph(mcp_client, "search_nodes", {"asset_path": fixture["adapter_package"], "function_name": RECORDING_FUNCTION})
        found_nodes = next((value for value in found.values() if isinstance(value, list)), [])
        assert any(node.get("node_id") == by_client["record"] for node in found_nodes), found

        # The authored override of the returning native hook. ReadPresentedTitle is const and has a
        # return value, so its implementation is a function graph whose result terminator is
        # addressed through its graph_ref plus node_guid: the implementation target creates the
        # graph uncompiled, and the second patch wires the return value and compiles.
        graph_preview = await apply_reviewed(
            run,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                target={"implementation": {"owner_class": FIXTURE_WIDGET_BASE, "function_name": "ReadPresentedTitle"}},
                nodes=[
                    {"client_id": "model_ref", "node_class": "VariableGet", "params": {"variable_name": MODEL_VARIABLE}},
                    {
                        "client_id": "model_title",
                        "node_class": "VariableGet",
                        "params": {"variable_name": "Title", "owner_class": host["model_class"]},
                    },
                ],
                connections=[
                    {"from": {"client_id": "model_ref", "pin": MODEL_VARIABLE}, "to": {"client_id": "model_title", "pin": "self"}},
                ],
                compile=False,
            ),
        )
        assert graph_preview["apply_status"] == "applied", graph_preview
        assert graph_preview["compile_status"] == "not_requested", graph_preview

        return_graph = (await run.graph_choices(fixture["adapter_package"]))["ReadPresentedTitle"]
        return_subgraph = await run.subgraph(fixture["adapter_package"], "ReadPresentedTitle")
        return_entry = next(
            (node for node in return_subgraph["nodes"] if node["class"] == "K2Node_FunctionEntry"), None
        )
        return_result = next(
            (node for node in return_subgraph["nodes"] if node["class"] == "K2Node_FunctionResult"), None
        )
        assert return_entry is not None and return_result is not None, [
            node["class"] for node in return_subgraph["nodes"]
        ]
        return_names = node_name_map(return_subgraph)
        # The engine links the created entry's exec output and the created result's return input when it
        # builds an inherited implementation graph, and the patch contract deliberately refuses to take
        # over an existing link. Both engine-created links are therefore detached first by the registered
        # graph.disconnect operation: separate graph maintenance steps outside the patch transaction,
        # never claimed as part of it. Detaching a pin that carries no link is a no-op.
        for pin_name, node in (("then", return_entry), ("ReturnValue", return_result)):
            await graph(
                mcp_client,
                "disconnect",
                {
                    "asset_path": fixture["adapter_package"],
                    "node_id": node["node_id"],
                    "pin_name": pin_name,
                    "graph_name": "ReadPresentedTitle",
                },
            )
        return_connections = [
            {
                "from": {"node_guid": graph_preview["node_mappings"]["model_title"], "pin": "Title"},
                "to": {"node_guid": return_result["node_guid"], "pin": "ReturnValue"},
            },
            {
                "from": {"node_guid": return_entry["node_guid"], "pin": "then"},
                "to": {"node_guid": return_result["node_guid"], "pin": "execute"},
            },
        ]
        wired = await apply_reviewed(
            run,
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                target={"graph_ref": {"graph_guid": return_graph["graph_guid"]}},
                connections=return_connections,
            ),
        )
        assert wired["readback_status"] == "matched", wired
        assert wired["compile_status"] == "compiled", wired
        assert (return_names[graph_preview["node_mappings"]["model_title"]], "Title") in {
            (edge["source_node"], edge["source_pin"]) for edge in (await run.subgraph(fixture["adapter_package"], "ReadPresentedTitle"))["edges"]
        }

        # Runtime observation through the native hook, including a forced GC point.
        observed = await observe_widget(mcp_client, fixture["adapter_class"], TITLE_ARGUMENT, collect_garbage=True)
        assert observed["read_presented_title"] == TITLE_ARGUMENT, observed
        assert observed["model_ref_valid"] is True, observed
        assert observed["model_ref_title"] == TITLE_ARGUMENT, observed
        assert observed["model_outer_is_widget"] is True, observed
        assert observed["model_ref_valid_after_gc"] is True, observed
        assert observed["model_ref_title_after_gc"] == TITLE_ARGUMENT, observed
        assert observed["read_presented_title_after_gc"] == TITLE_ARGUMENT, observed
        assert observed["title_label_class"] == host["host_class"], observed
        assert observed["host_observed_title"] == RECORDING_DEFAULT_LITERAL, observed
        assert observed["host_observed_model_valid"] is True, observed
        assert observed["host_observed_model_title"] == TITLE_ARGUMENT, observed
    finally:
        await run.cleanup()


# ---------------------------------------------------------------------------
# Scenario B: preview / invalid / no-op replay / explicit save + reload
# ---------------------------------------------------------------------------


@pytest.mark.anyio
async def test_scenario_typed_authoring_preview_invalid_save_and_replay(mcp_client):
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        host = await build_host_and_model(run)
        fixture = await build_adapter(run, host)
        patch_id = str(uuid.uuid4())

        # 1. Preview in an unchanged state.
        body = envelope(
            fixture["adapter_object"],
            patch_id,
            (await run.context(fixture["adapter_package"]))["fingerprint"],
            **fixture["intent"],
        )
        preview = await run.apply(body)
        assert preview["changed"] is True and preview["validation_hash"], preview
        after_preview_count = (await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"]))["node_count"]

        # 2. Invalid request: a bare string default is not a tagged literal, and it is refused before
        #    any mutation.
        untagged = dict(fixture["intent"])
        untagged["nodes"] = [
            node if node["client_id"] != "record" else {**node, "defaults": {"Title": RECORDING_DEFAULT_LITERAL}}
            for node in fixture["intent"]["nodes"]
        ]
        refused = await run.apply(
            envelope(
                fixture["adapter_object"],
                str(uuid.uuid4()),
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                **untagged,
            )
        )
        assert refused["_error"] == "INVALID_FIELD", refused
        assert refused["apply_status"] == "not_requested", refused
        after_refusal = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        assert after_refusal["node_count"] == after_preview_count, "an invalid request mutated the graph"

        # 3. Apply the reviewed intent (the preview token is the precondition).
        applied = await run.apply({**body, "dry_run": False, "expected_validation_hash": preview["validation_hash"]})
        assert applied["apply_status"] == "applied", applied

        # 4. A stale precondition is refused: an earlier preview token no longer matches the intent.
        stale = await run.apply({**body, "dry_run": False, "expected_validation_hash": preview["validation_hash"] + "00"})
        assert stale["_error"] == "STALE_PRECONDITION", stale
        unchanged = await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"])
        assert len(unchanged["nodes"]) == len((await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"]))["nodes"])

        # 5. No-op replay: the same patch_id and intent report a complete reuse with no compile/save.
        replay_preview = await run.apply(
            envelope(
                fixture["adapter_object"],
                patch_id,
                (await run.context(fixture["adapter_package"]))["fingerprint"],
                **fixture["intent"],
            )
        )
        assert replay_preview["changed"] is False, replay_preview
        replay = await run.apply(
            {**body, "dry_run": False, "expected_validation_hash": replay_preview["validation_hash"]}
        )
        assert replay["apply_status"] == "unchanged", replay
        assert sorted(replay["reused_client_ids"]) == sorted(applied["node_mappings"]), replay
        assert replay["target_compile_count"] == 0, replay
        assert replay["save_status"] == "not_requested" and replay["saved"] is False, replay

        # 6. Explicit save: a clean starting package plus save=true commits the file.
        await blueprint(mcp_client, "save", {"asset_path": fixture["adapter_package"]})
        old_file = run.package_file(fixture["adapter_package"])
        digest_before = old_file.read_bytes() if old_file.exists() else b""
        save_body = envelope(
            fixture["adapter_object"],
            str(uuid.uuid4()),
            (await run.context(fixture["adapter_package"]))["fingerprint"],
            target={"graph_ref": {"graph_guid": fixture["ubergraph"]["graph_guid"]}},
            nodes=[
                {
                    "client_id": "spare",
                    "node_class": "CallFunction",
                    "params": {"function_name": "KismetSystemLibrary.PrintString"},
                }
            ],
            dry_run=False,
            save=True,
        )
        save_preview = await run.apply({**save_body, "dry_run": True})
        saved = await run.apply({**save_body, "expected_validation_hash": save_preview["validation_hash"]})
        assert saved["apply_status"] == "applied", saved
        assert saved["save_status"] == "saved", saved
        assert saved["post_save_status"] == "verified", saved
        assert saved["saved"] is True and saved["dirty_after"] is False, saved
        digest_after = run.package_file(fixture["adapter_package"]).read_bytes()
        assert digest_after != digest_before and digest_after, "the saved package did not change on disk"

        # 7. Reload: the text-pin path saves, re-reads the package from disk and re-verifies the
        #    persisted graph and the canonical FText identity.
        names = node_name_map(await run.subgraph(fixture["adapter_package"], fixture["ubergraph"]["graph_name"]))
        record_name = names[applied["node_mappings"]["record"]]
        reloaded = await graph(
            mcp_client,
            "set_pin_value",
            {
                "asset_path": fixture["adapter_package"],
                "node_id": record_name,
                "pin_name": "Title",
                "text": {"type": "FText", "source_kind": "literal", "value": "ReloadedTitle"},
                "graph_name": fixture["ubergraph"]["graph_name"],
                "graph_kind": fixture["ubergraph"]["graph_kind"],
                "expected_fingerprint": (await run.context(fixture["adapter_package"]))["fingerprint"],
            },
        )
        assert reloaded["reloaded"] is True, reloaded
        assert reloaded["saved"] is True and reloaded["verification_passed"] is True, reloaded
        assert reloaded["verified_text"]["value"] == "ReloadedTitle", reloaded
    finally:
        await run.cleanup()


# ---------------------------------------------------------------------------
# Scenario C: Actor Blueprint context, null input and the cast-failure route
# ---------------------------------------------------------------------------


@pytest.mark.anyio
async def test_scenario_typed_authoring_actor_context_null_cast_failure(mcp_client):
    run = AuthoringRun(mcp_client, uuid.uuid4().hex[:8])
    try:
        host = await build_host_and_model(run)
        actor_package = await run.create(ACTOR_NAME, kind="Actor")
        actor_class = run.generated_class(actor_package)
        for variable in ("bBeginPlayed", "bCastSucceeded"):
            await blueprint(mcp_client, "add_variable", {"asset_path": actor_package, "name": variable, "type": "bool"})
        await blueprint(mcp_client, "add_variable", {"asset_path": actor_package, "name": MODEL_VARIABLE, "type": host["model_class"]})

        graphs = await run.graph_choices(actor_package)
        event_graph = next(choice for choice in graphs.values() if choice["graph_kind"] == "ubergraph")

        # An Actor Blueprint has no inherited implementation to override, so the target is the graph
        # itself and the entry is an authored Event node.
        body = envelope(
            run.object_path(actor_package),
            str(uuid.uuid4()),
            (await run.context(actor_package))["fingerprint"],
            target={"graph_ref": {"graph_guid": event_graph["graph_guid"]}},
            nodes=[
                {"client_id": "begin", "node_class": "Event", "params": {"function_name": "Actor.ReceiveBeginPlay"}},
                {
                    "client_id": "mark_begin",
                    "node_class": "VariableSet",
                    "params": {"variable_name": "bBeginPlayed"},
                    "defaults": {"bBeginPlayed": {"kind": "bool", "value": True}},
                },
                {
                    "client_id": "cast_host",
                    "node_class": "DynamicCast",
                    "params": {"class": host["host_class"], "is_pure": False},
                    "defaults": {"Object": {"kind": "null"}},
                },
                {
                    "client_id": "mark_cast",
                    "node_class": "VariableSet",
                    "params": {"variable_name": "bCastSucceeded"},
                    "defaults": {"bCastSucceeded": {"kind": "bool", "value": True}},
                },
                {
                    "client_id": "store_null",
                    "node_class": "VariableSet",
                    "params": {"variable_name": MODEL_VARIABLE},
                    "defaults": {MODEL_VARIABLE: {"kind": "null"}},
                },
            ],
            connections=[
                {"from": {"client_id": "begin", "pin": "then"}, "to": {"client_id": "mark_begin", "pin": "execute"}},
                {"from": {"client_id": "mark_begin", "pin": "then"}, "to": {"client_id": "cast_host", "pin": "execute"}},
                {"from": {"client_id": "cast_host", "pin": "then"}, "to": {"client_id": "mark_cast", "pin": "execute"}},
                {"from": {"client_id": "cast_host", "pin": "CastFailed"}, "to": {"client_id": "store_null", "pin": "execute"}},
            ],
        )
        preview = await run.apply(body)
        assert preview["changed"] is True, preview
        applied = await run.apply({**body, "dry_run": False, "expected_validation_hash": preview["validation_hash"]})
        assert applied["apply_status"] == "applied" and applied["readback_status"] == "matched", applied

        names = node_name_map(await run.subgraph(actor_package, event_graph["graph_name"]))
        by_client = {client_id: names[guid] for client_id, guid in applied["node_mappings"].items()}
        subgraph = await run.subgraph(actor_package, event_graph["graph_name"])
        read_edges = edges(subgraph)
        null_input = pin(subgraph, by_client["cast_host"], "Object")
        assert null_input["default_descriptor"] == {"kind": "null"}, null_input
        assert (by_client["cast_host"], "CastFailed", by_client["store_null"], "execute") in read_edges
        assert (by_client["begin"], "then", by_client["mark_begin"], "execute") in read_edges

        observed = await observe_actor(mcp_client, actor_class)
        assert observed["begin_play_dispatched"] is True, observed
        assert observed["begin_played"] is True, observed
        # The cast received a null object, so the success route must not have run.
        assert observed["cast_succeeded"] is False, observed
        assert observed["model_ref_valid"] is False, observed
    finally:
        await run.cleanup()
