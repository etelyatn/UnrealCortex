# Issue 277 design: migration inspection and graph-edit papercuts

**Request:** [UnrealCortex#277](https://github.com/etelyatn/UnrealCortex/issues/277), follow-up to #274: four bugs and
seven inspection/authoring requests from removing forwarding graphs during a Widget Blueprint to C++ migration
(plugin 0.4.20). Triage against `main` 0.4.30 (`3970d1c2`): every item is still open in source; item 9's read half is
already available through `graph.get_subgraph` `compact: false`, but undiscoverable. Delivery: every item.

## Decisions

### A. Blueprint domain (items 1, 2, 4, 5, 6, 8)

| Item | Contract |
|---|---|
| 1 `remove_interface` `preserve_implementations: false` | After the engine removes the interface event nodes, Cortex removes each event's island in the same guarded run: the exec chain driven by the event plus the pure/data nodes that only feed it. The removal is conservative: a node reached by another live entry (or still driven/consumed by a kept node) is kept as `shared`; timelines, macro instances, collapsed-graph (composite) nodes and Add Component nodes are never deleted (`timeline`, `macro_instance`, `collapsed_graph`, `component_template`) and the walk stops at them; an interface event that lives inside a collapsed graph deletes nothing (`collapsed_graph`). Per function the plan and the response report `removed_node_guids` (`disposition: removed_with_body`, or `deleted` when nothing is removed) and `retained_node_guids: [{node_guid, reason}]`. Island nodes and both ends of their links get `RF_Transactional` + `Modify()` before the engine call, so editor undo restores nodes and links. The plan is part of the validation hash, so the preview shows exactly what apply deletes. `preserve_implementations: true` is unchanged. |
| 2 `remove_graph` apply token | `expected_validation_hash` alone authorizes apply: the hash already commits to the live fingerprint at preview time. `expected_fingerprint` becomes optional on apply and is still checked when supplied. A missing hash → `STALE_PRECONDITION` naming `expected_validation_hash` and the dry run's `validation_hash` field. The same rule applies to `blueprint.remove_interface` and `blueprint.replace_interface`. `replace_interface` fills the live fingerprint into the shared graph patch when only the hash is given; the graph migration hashes now commit to the live fingerprint after validating a supplied one, so a partial supplied fingerprint no longer changes the hash. |
| 4 `blueprint.search` | Variable get/set nodes match by variable (member) name in the `pins` area: match `type: "variable_get"` / `"variable_set"`, `property` = member name, `location` = `<graph>/<node>`, `node_id`. Same `max_results` cap and order. |
| 5 variable references | New read-only `blueprint.find_variable_references`: `asset_path`, `variable` (Blueprint variable or inherited/native property name). Response: `variable`, `declared_in` (owner class path, or the Blueprint), `is_native`, `get_count`, `set_count`, `references: [{kind: get|set, node_id, node_guid, graph_name, graph_guid, subgraph_path, entries: [{name, node_guid}], consumers: [{node_guid, node_class, title, pin}]}]`. Entries are the live entries that reach the node, with the graph-metrics seeding (pure function results included) and carried through collapsed graphs in both directions (a getter in a collapsed body reports the outer event; an event collapsed into a composite is the entry of the nodes behind the composite). Consumers are the distinct nodes on the node's output links (through reroutes). Unknown variable → `INVALID_FIELD`. Matching resolves a copy of the member reference (redirects, member GUID) and compares the resolved property and owner. |
| 6 `blueprint.get_info` interfaces | Response adds `interfaces: [{name, path, is_blueprint, implementation_graphs: [names]}]` from `ImplementedInterfaces` (inherited interfaces of the parent are not listed; `inherited: false` is not a field). |
| 8 `graph_metrics` | `totals` adds `entry_nodes`, `result_nodes`, and `all_nodes` (the unique authored nodes of the Blueprint's own graphs, collapsed bodies and tunnels included, comments and reroute knots excluded; counted directly, independent of `functional_nodes`, with depth cuts reported as truncation), beside the unchanged `functional_nodes`. New `asset_paths` (array, exclusive with `asset_path`): response `{assets: [payload | {asset_path, error: {code, message}}], count}` with one per-asset payload identical to the single-asset response. |

### B. Graph domain (items 3, 9, 10, 11)

| Item | Contract |
|---|---|
| 3 `get_authoring_context` | Accepts `graph_name` (top level, or `target.graph_name` without `graph_ref`), resolved like the other graph reads through `FCortexGraphTree::ResolveInTree` after the Blueprint loads. A name matching several graphs → `INVALID_FIELD` with `details.candidates: [{graph_guid, graph_name, graph_kind, subgraph_path}]`; a name matching only nested graphs returns the resolver's `nested_candidates`. `graph_ref` keeps precedence. |
| 9 positions | Read: `graph.get_subgraph` already returns `position` with `compact: false`; the description says so. Write: `graph.add_node` accepts `placement: {relative_to: node_id, side: left|right|above|below, gap?: int = 80}`, exclusive with `position`. The new node is placed beside the anchor's body (estimated size, same estimate as auto-layout). Response reports the final `position`. Unknown anchor → `NODE_NOT_FOUND`; a coordinate outside int32 range (conservative node extent for left/above) → `INVALID_FIELD`; nothing is added on either refusal. |
| 10 getter fan-out | New `graph.split_getter`: `asset_path`, `node_id`, optional `graph_name`, `subgraph_path`, `dry_run`. For a pure `K2Node_VariableGet` whose output feeds several distinct consumer nodes, keeps the first consumer (by link order) on the original node and gives every other consumer its own get node (same variable reference and self/target pin wiring), placed left of that consumer. One transaction. Refused with `INVALID_OPERATION`: impure variations (`branch`, `validated_object`), a split struct output pin (recombine first), a reference whose fresh getter cannot reproduce the output pin, a blocked asset. Fewer than two consumers → success with `created: []`, `changed: false`. Response: `original_node_id`, `original_node_guid`, `created: [{node_id, node_guid, consumer_node_id, consumer_node_guid}]`, `consumer_count`, `changed`. |
| 11 override pre-flight | `graph.remove_nodes` (preview and apply) reports `override_events` for each `K2Node_Event` with `bOverrideFunction` in the removal set: `node_id`, `node_guid`, `function`, `declaration_resolved`, `declared_in`, `native_declaration`, `has_native_default` (removing the override falls back to a reachable native `_Implementation`; for an interface declaration only when a native ancestor implements the interface), `blueprint_callers` (loaded call nodes whose resolved function and owner match, removed nodes excluded: `asset_path`, `graph_name`, `node_guid`, `node_id`), `blueprint_callers_complete` (true only when every asset-registry referencer of every package in the class chain up to the declaring class is loaded and the declaration resolved), `unloaded_referencers` (sorted, capped at 50) + `unloaded_referencer_count`, and `native_callers: "not_enumerable"` (C++ call sites are not discoverable from the editor). The scan resolves copies of references and never rewrites nodes. Informational; never blocks the removal. |

### C. Reflect domain (item 7)

| Item | Contract |
|---|---|
| 7 | `reflect.find_interface_implementers` adds, when `include_callers` is true, `referencers: {implements: [asset_path], calls_or_messages: [asset_path], other: [asset_path]}`: the interface package's hard asset-registry referencers classified by the evidence the command already computes (implementer tag; resolved caller sites), per asset even inside multi-asset packages. An asset that both implements and calls appears in both. With `out_path` the inline summary carries `implements_count`, `calls_or_messages_count`, `other_count`. New optional `implementer` (asset path) narrows `implementers` to that asset and adds per interface function `implementer_handles` resolved child-first up to the declaring ancestor (`via`); unproven negatives are omitted when a level fails to load (`load_failures`). The callers list stays global because interface dispatch targets are runtime values; the description states that limit. |

## Non-goals

- No static "which callers reach implementer X" dataflow (runtime dispatch); the split in item 7 is the requested fallback.
- No C++ call-site enumeration (item 11 reports it as not enumerable).
- `compact` keeps omitting positions (existing contract; tests pin it).

## Shared contracts and generated artifacts

- Every new param/command is declared in `GetSupportedCommands`; the router rejects undeclared params.
- `MCP/tests/fixtures/capabilities_cache_full.json` and `_fallback_generated.py` regenerate once at integration.
- System docs in CortexSandbox `docs/systems/` and the toolkit surface fixture update at integration.
