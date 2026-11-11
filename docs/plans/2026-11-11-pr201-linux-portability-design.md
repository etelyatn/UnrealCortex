# PR #201 Linux build and Unix SHA-256 design

## Value and route

Patch and merge @Boshchuk's PR #201 (`8d92d99db273de2517c93f3edccf7b96be1db977`). Two independent defects on non-Windows hosts:

1. **Clang build break.** UE 5.8 keys `FJsonObject::Values` by `UE::TSharedString`. A range-for that binds elements to `const TPair<FString, TSharedPtr<FJsonValue>>&` builds a temporary per iteration; MSVC accepts it, Clang rejects it with `-Werror,-Wrange-loop-construct`. The contributor fixed 15 sites; `main` gained a 16th (`CortexReplayFormat.cpp` `HasOnlyKeys`, from #202) after the PR's base.
2. **Replay and physical-input SHA-256 off Windows.** `FCortexReplaySha256State` and `CortexEditorPhysicalInputSha256` had only a BCrypt branch, so every recording publish failed on Linux and physical-input digests were empty.

Route: retain the contributor's design, merge current `main`, add bounded maintainer corrections, decline one slice (below).

## Decisions

- Loops use `const auto&` and convert the key once into a local via `CortexEngineCompat::JsonKeyToString`, matching existing call sites (e.g. `CortexBPMigrationGuard.cpp`). No behavior change.
- Unix SHA-256 reuses CortexCore's #143 wiring: `UnrealPlatformGroup.Unix` selects the engine's private OpenSSL module (1.1.1t on UE 5.8) and defines `CORTEX_SHA256_OPENSSL`. Windows keeps BCrypt; Apple and other platforms keep the explicit unsupported error. Hex digests use the allocation-free formatter from #143.
- New known-answer tests (`Cortex.Replay.Library.Sha256KnownAnswers`, `Cortex.Editor.PhysicalInputSha256KnownAnswers`) pin FIPS 180-2 vectors on whichever backend the platform compiles.
- **Declined:** the contributor's `Cortex.Core.SourcePortability.JsonObjectKeyLoops` test. It scans source text line by line, so it asserts spelling rather than behavior, misses multi-line or reordered declarations, and warns in binary installs. The regression is caught by compiling with Clang; a Clang compile gate is tracked separately.

No command, parameter, response field, schema, or toolkit change.
