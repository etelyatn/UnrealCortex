# Fixture-dependent automation crash prevention (#150)

## Requirement and validation

Missing host-project fixtures must produce automation failures, not terminate the editor. Current animation inspection tests assert `Result.Data.IsValid()` without branching before dereferencing it. CodexTranslation likewise continues after failed file/JSON assertions and dereferences `RootObject` and `ServersObject`.

## Decision

Preserve existing failure semantics and fixture coverage. Return false after prerequisite assertions fail. Guard all four animation response pointers and the config test's file existence, load, parse, argument count, flag/value parity, and servers-object prerequisites. Check nested config object validity before dereferencing. No production changes, fixture substitution, or silent skips.

## Acceptance

Existing Sandbox fixtures retain their assertion coverage. In a bare host project without mannequin assets or `.mcp.json`, the five affected tests report failures and automation continues to completion without a shared-pointer assertion crash.
