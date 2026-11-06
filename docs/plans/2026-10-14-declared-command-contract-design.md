# Declared command contract enforcement — design

**Date:** 2026-10-14
**Status:** Implemented
**Context:** MCP benchmark remediation (`CortexSandbox/Saved/mcp_benchmark_issues_20261014_075611.md`, a CortexSandbox project-repo path)

## Problem

`FCortexCommandInfo` declares every routed command's parameters through `Required()` and `Optional()`
(497 required, 526 optional across the plugin) and those declarations are published to callers through
`get_capabilities` and `get_operation_schema`. Nothing enforced them: the router forwards `params`
verbatim and validation is opt-in per handler, so a command could be called with a parameter it never
reads, or without a parameter it declares required, and still run and report success.

Three benchmark findings were the same defect seen from three angles:

- `umg.set_padding {top: 16}` reported `set: true` while applying a zero margin — `padding` is the
  declared (nested) parameter and named nothing else, so the flat `top` was dropped.
- `umg.set_anchor {anchor: "Center"}` reported `set: true` while leaving the anchors untouched —
  `anchor` is not a parameter at all; the anchor is named `preset`.
- `data.list_data_assets {class_filter: "InputAction"}` returned an unfiltered result set — the
  selector had been renamed to `class_name` and the retired name was silently unread.

The last one is the most damaging shape: the caller cannot tell a filtered read from an unfiltered one.

## Design

The published declaration becomes the single source of truth.

- `FCortexCommandInfo` gains `bStrictParams` and the builder `StrictParams()`, mirroring the existing
  `RollbackSafe()` opt-in.
- `FCortexCommandContract::ValidateParams` enforces a declaration: it refuses any parameter the
  command does not declare (`INVALID_FIELD`, naming the offending parameter and the accepted set) and
  any call missing a declared-required parameter (`INVALID_FIELD`, naming the parameter and its
  declared type).
- `FCortexCommandContract::ValidateDeclaredParams` looks a command up in a domain's declaration table
  and validates only when that command opted in; an unopted command, or one absent from the table
  (the router reports unknown commands itself), is unaffected.
- Each domain calls it once at the top of its `Execute`, before any load, transaction or mutation, so
  a refused call can never have partially applied.

### Semantics (deliberate)

- **Declared types are not enforced.** The type string is documentation, not a machine contract, and a
  value may legitimately be accepted by a declared "object": `set_padding` takes either
  `{left, top, right, bottom}` or a uniform number. Enforcing the string would refuse a working call.
- **Presence is tested, not non-null.** An explicit `null` is a meaningful value for commands that
  clear state (e.g. `umg.set_property_binding`), so `HasField` is the presence test.
- **Router-owned keys are exempt.** Parameters injected by the MCP layer carry a leading underscore
  (e.g. `_telemetry_parallel`) and are always accepted.
- **Only the command's own top-level names are checked.** The validator does not recurse into nested
  objects or arrays, and it does not verify that an underscore-prefixed key actually came from the
  router — a caller could supply one and have it ignored.
- **Semantic constraints stay in handlers.** A schema cannot express "this value must be a known preset
  token" or "name at least one settable field", so those checks live in the handler — still before the
  transaction and any mutation, and they no longer report `set: true` for a request they ignored.
- **Opting in requires an accurate declaration.** Two commands (`set_anchor`, `set_font`) declared
  fewer keys than their handlers read; enforcing a stale declaration would have refused a working
  parameter, so their declarations were completed first.

### Rejected alternative

Enforcing the declared contract centrally for every routed command is the same one-line opt-in in
`FCortexCommandRouter::Execute`, but it changes acceptance for ~200 commands, including downstream
automation outside this repository. It is recorded as `TD-MCP-005` in the CortexSandbox project repo
(`CortexSandbox/docs/tech-debt/cortex-mcp-tech-debt.md` — project-side debt lives there, not in this
plugin repo) with the audit and replay evidence it requires first; the
`set_anchor`/`set_font` mismatches are the proof that the published metadata is not yet trustworthy in
general.

## Consequences

- The capability payload changes intentionally for the completed declarations (`set_anchor` gains
  `min`/`max`/`alignment`/`offset` and `preset` becomes optional; `set_font` gains
  `outline_size`/`outline_color`; `set_padding`'s `padding` description states both accepted forms).
  The Python mirror `MCP/tests/fixtures/capabilities_cache_full.json` and the derived
  `_fallback_generated.py` were migrated with it.
- `widget_compose` emitted a flattened padding dict through the same silent-drop route; it now emits
  the declared nested object.
- An existing test encoded the old silent-drop contract for `class_filter`; it now asserts the refusal.
