# Issue 255: enforce declared command parameters for every routed command (design)

**Date:** 2026-11-19
**Status:** Implemented
**Request:** <https://github.com/etelyatn/UnrealCortex/issues/255> (CortexSandbox `TD-MCP-005`)
**Builds on:** `2026-10-14-declared-command-contract-design.md`

## Problem

`FCortexCommandInfo` declares every command's parameters, and `get_capabilities` and
`get_operation_schema` publish those declarations. Only commands that opt in with `StrictParams()` had
them enforced, and only in the four domains that called `FCortexCommandContract::ValidateDeclaredParams`
themselves (Blueprint, Data, Reflect, UMG). The `StrictParams()` markers in Core, Editor and Level were
dead, because nothing in those handlers called the validator. Every other command silently ignores a
parameter it does not read, so a renamed or mistyped key looks like success. Issue #234 item 1 is an
example: `editor.get_recent_logs` with two non-existent params returned 200 unfiltered rows.

## Decision

The router owns the contract. `FCortexCommandRouter::Execute` resolves the domain, finds the command's
declaration, and validates before the handler runs. Batch steps reach domains through the same
`Execute`, so they are covered too.

| Check | Applies to | Error |
|---|---|---|
| Every top-level key is declared, or starts with `_` | every declared command of every registered domain | `INVALID_FIELD`: `Unknown param 'x'. Accepted params: a, b` (or `none` for a command that declares no params). Keys longer than 64 characters are not echoed: `Unknown param (name length N)` |
| Every declared-required key is present (`HasField`, so `null` counts) | commands that opt in with `StrictParams()` | `INVALID_FIELD`: `Missing required param: x (type)` |

- **Unknown-key rejection is universal.** That is what the issue asks for, and it is safe once the
  declarations are complete. A command that declares no params rejects any non-underscore key.
- **Required presence stays opt-in.** Several commands accept alternative shapes, for example
  single-target fields or batch `items`. A `Required()` marker therefore describes the usual call, not
  an invariant the handler relies on. Enforcing it for all ~340 commands is a second, larger acceptance
  change that the issue does not request. Handlers keep their own required checks. `StrictParams()`
  keeps its name and now means "also enforce declared-required presence".
- **The command is not in the table.** The handler runs unchanged and reports its own unknown-command
  error, so the router never invents a second error shape. This only holds if every dispatched name is
  declared. The audit found one undeclared dispatched alias, `graph.authoring_context`. Nothing used
  it, so it is removed rather than declared.
- **Built-ins are out of scope.** `ping`, `get_status`, `get_capabilities` and `batch`/`batch_query` are
  not domain commands and validate their own input. `core.batch_query` is declared only so that MCP can
  discover its schema, because MCP translates it to `batch`. Native Core does not dispatch it. A raw
  `core.batch_query` therefore still reports `UNKNOWN_COMMAND`, unless it carries an undeclared key; in
  that case the contract refusal comes first.
- **Domain refusal envelopes are kept.** `graph.apply_patch` reports every terminal outcome, including
  refusals, in its compact patch-outcome envelope, and MCP forwards that envelope. A domain can enrich a
  router refusal through `ICortexDomainHandler::DecorateParamRefusal(Command, Params, InOutRefusal)`,
  which by default does nothing. Graph uses it to attach the unloaded-refusal outcome. The router still
  decides and still owns the message. The domain adds details but cannot accept the call.
- **Router-owned keys stay exempt.** Keys with a leading underscore (`_telemetry_parallel`) are not
  checked. As before, the validator does not verify that such a key really came from the router. The
  exemption covers only the router's check. Commands that already enforced their own closed key sets
  before this change still refuse `_` keys with their existing errors: `graph.apply_patch`,
  `blueprint.remove_interface`, and the Replay commands. Neither TCP nor the MCP transport injects
  such keys into native params.
- **Types and nesting are not enforced.** This is unchanged from the 2026-10-14 design.
- **Validation runs before the handler**, so a refused call has no load, transaction or mutation. A
  deferred command that is refused returns a synchronous error, and its callback is never taken.
  Inside a batch, the refused step's handler never runs. The batch transaction, sibling steps and
  rollback still follow the batch flags.

### Lookup cost

Each `FCortexRegisteredDomain` caches its declarations at `RegisterDomain` time in a
`TMap<FString, FCortexCommandInfo>`. Every `GetSupportedCommands()` implementation returns a static
table, so a one-time snapshot is exact. Per-command validation is then one map lookup plus a scan of the
request's keys, with no per-call rebuild of the ~40-entry declaration array. `IsRollbackSafeCommand`
reuses the same lookup.

### Declaration correctness (precondition)

Universal rejection is only correct if every declaration lists every top-level key its handler reads.
Each domain's declaration table is audited against the keys its handler and operations read, both
directly (`TryGet*Field`, `Has*Field`, `Get*Field`) and through shared helpers. Each command reaches one
of three outcomes:

1. **The handler reads a key that is not declared.** Declare it, `Optional` unless it is truly
   required, with an accurate description.
2. **A key is declared but never read.** Remove it from the declaration. A caller that sends it now gets
   a refusal instead of a silent no-op, which is the purpose of the change.
3. **A command accepts open-ended top-level keys.** No such command is permitted. If one exists, its
   open part moves under a declared object parameter, and its callers move with it.

Python MCP wrappers, composite builders and scenario suites are audited for keys they send that the
corrected declaration does not accept. Every such caller is fixed in the same change.

## Consequences

- Behaviour change for callers. A routed command that receives an undeclared top-level key now fails
  with `INVALID_FIELD` instead of ignoring the key. Downstream automation that relied on silent ignore
  must drop or rename the key. The error lists the accepted names.
- `ValidateDeclaredParams` and the four per-domain call sites are removed. The router is the only
  enforcement point.
- The capability payload changes wherever a declaration was corrected. The Python mirror
  (`MCP/tests/fixtures/capabilities_cache_full.json`) and `_fallback_generated.py` are regenerated from
  the live editor cache.

## Verification

- A native Core test registers a probe domain and checks four behaviours through the real router.
  Unknown keys are rejected for an unopted command. `_` keys are accepted. A command that declares no
  params rejects a key. Required presence is checked only for a `StrictParams()` command. A batch step
  that carries an unknown key fails at that step, while its sibling step succeeds.
- The full native suites run with enforcement on. Any test whose router call now fails is either a
  declaration gap (fix the declaration) or a test sending a stale key (fix the test).
- Python `not e2e` tests, then e2e and scenario tests against a live editor, and the live MCP
  benchmark, all run with enforcement on.
