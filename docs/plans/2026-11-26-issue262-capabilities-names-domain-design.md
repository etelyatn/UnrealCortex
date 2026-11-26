# Issue 262 — capability names view and domain selector

**Issue:** https://github.com/etelyatn/UnrealCortex/issues/262
**Owner:** UnrealCortex / Python MCP facade (`core_cmd get_capabilities`)

## Problem

`core_cmd get_capabilities` refuses with `RESPONSE_TOO_LARGE` and advises `detail=names`, but the
names view refused the same way. The projection dropped only each command's `params`; it kept
every command's `description`, and the live inventory (~118k compact characters in full) still
projected above the 40,000-character budget (the recorded fixture alone projects to ~56k). There
was also no way to list one domain's commands (`material.list_commands` is `UNKNOWN_COMMAND`).

## Decision

Both selectors stay Python-owned presentation on the existing route; native `get_capabilities`
is unchanged and never receives them.

- `detail=names` (default): each domain's `commands` is the list of command **name strings**.
  Domain fields (`name`, `version`) and top-level fields (`plugin_version`) are kept. The recorded
  fixture's names view is ~6k characters.
- `domain=<registered domain>` (new, optional): narrows either view to that one domain.
  `detail=full, domain=X` returns X's complete signature inventory. An unregistered or non-string
  domain refuses with `INVALID_FIELD` and lists the registered domains.
- The oversize refusal for `detail=full` advises `detail=names`, `domain=<name>`, or
  `get_operation_schema` for one operation. A single domain's full inventory can still exceed the
  budget (Blueprint, ~48k in the fixture); that case refuses complete-or-nothing like any other
  full read, and `get_operation_schema` is the per-operation route.

## Rejected

- **`list_commands` in every domain handler:** a second discovery route per domain duplicating
  the capabilities document; `domain` on the existing route covers it.
- **Paging the names view:** the names-only list fits the budget; paging adds a cursor contract
  for no need.
- **Native `domain` parameter:** the native document is cached whole by the connection; filtering
  after the cached read keeps one cache entry and one native contract.

## Compatibility

The names view's command entries change from `{name, description}` objects to strings. Callers
needing descriptions or signatures use `detail=full` (optionally with `domain`) or
`get_operation_schema`. The Saved capabilities cache and the connect handshake are untouched.
