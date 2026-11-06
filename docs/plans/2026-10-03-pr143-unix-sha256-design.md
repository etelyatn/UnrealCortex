# PR #143 Unix file SHA-256 design

## Value and route

Retain Boshchuk's PR #143 (`01863a33fd56005dd64bf38069413f2b3143c116`) implementation: Unix currently returns `INVALID_OPERATION` from `FCortexSafeFileContract::HashFileBytesSha256`, blocking file-backed import queues. Use Unreal's existing private OpenSSL module for the Unix platform group. Windows retains BCrypt; Apple and other non-Unix platforms retain the explicit unsupported-platform error. No new command, parameter, response field, public helper signature, retry, fallback, or implicit save.

The engine's OpenSSL build rules select Unix headers/libraries with the same `UnrealPlatformGroup.Unix` predicate. Generic `GetSHA256Signature` is not a replacement for a missing Unix implementation.

## Candidate

Initial branch `maintainer/pr143-unix-sha256` is based on the verified PR #142 candidate `026b89f`. Final publication branch `maintainer/pr143-final` preserves the same contributor merge and restored maintainer correction. Increment plugin version 16 / 0.3.4 to 17 / 0.3.5. The initial start-working instruction kept candidates local; subsequent explicit authorization selects merge and closure of both PRs, with native Linux review/testing requested in #143's comment.

## Implementation decision

Retain platform-conditional private dependency and explicit `CORTEX_SHA256_OPENSSL=0/1`. Preserve safe read-path resolution, canonical-file revalidation, read-error handling, and lowercase 64-character SHA-256 output. The new Unix formatter appends two hexadecimal characters per digest byte into pre-reserved output; no per-byte `FString::Printf` temporary allocations. Existing Windows formatting is unchanged.

Retain contributor known-answer coverage for exact `abc` bytes and empty input, asserting fixture success/size and FIPS digest literals independently of the implementation.

## Acceptance and verification limits

- Native Windows build and safe-file known-answer/read-hash tests pass; consumer import-queue tests pass.
- Exercise the real new OpenSSL body using a temporary host-only backend-selection change and engine OpenSSL dependency, retaining actual Unreal file resolution/read/digest/formatting and tests. Restore platform selection before final normal Windows rebuild/check.
- Linux UE editor/toolchain is not configured on this workstation. WSL Ubuntu has OpenSSL but no discovered C++ compiler or Linux UE editor; do not call Windows-host OpenSSL evidence a Linux UE build or native Linux import-queue proof. Contributor Linux UE5.8.2 RED/GREEN evidence remains attributed, not maintainer-exercised.
- No toolkit/API synchronization or MCP-domain benchmark is triggered by this internal backend change. System domain boundaries and durable formats remain unchanged.
