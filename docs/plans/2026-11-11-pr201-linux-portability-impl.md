# PR #201 Linux build and Unix SHA-256 implementation

## Files and invariants

- JSON key loops (`const auto&`, key converted once): `CortexBPAdoptMemberOps.cpp`, `CortexBPAdoptSCSOps.cpp`, `CortexBPGraphMetricsOps.cpp`, `CortexBPSwapSCSClassOps.cpp`, `CortexDataTypeReplacementOps.cpp`, `CortexReflectAssetEnumeration.cpp`, `CortexReplayFormat.cpp`, `CortexSTReplaceNodeOps.cpp`, `CortexUMGWidgetTreeOps.cpp`, plus tests `CortexBPAnalysisTest.cpp`, `CortexBPReplaceInterfaceTest.cpp`, `CortexReflectDependencyGraphTest.cpp`, `CortexUMGRenameDependentsTest.cpp`. Error messages, keys written back, and FName conversions are unchanged.
- `CortexReplay.Build.cs`, `CortexEditor.Build.cs`: Unix-only private OpenSSL dependency and `CORTEX_SHA256_OPENSSL=0/1`, identical to `CortexCore.Build.cs`. Replay keeps main's zlib dependency.
- `CortexReplayStorage.cpp`: OpenSSL branch of `FCortexReplaySha256State`. `HashHandle` owns a heap `SHA256_CTX`; `Begin` closes first, `Finish` and the destructor close, `Close` is idempotent. `Update` rejects inactive state and negative sizes; zero size is a no-op.
- `CortexEditorPhysicalInputGuards.cpp`: one-shot `SHA256()`; negative size or failure returns the empty string the callers already treat as unavailable.
- Tests: `Cortex.Replay.Library.Sha256KnownAnswers` (empty, `abc`, two-block, one million `a` through `ComputeSha256Hex`), `Cortex.Editor.PhysicalInputSha256KnownAnswers` (empty, `abc`, two-block).
- Removed: `CortexCore/Private/Tests/CortexSourcePortabilityTest.cpp` (contributor's source-text scan; see design).

## Verification approach

- Clang 18 `-fsyntax-only -Werror=range-loop-construct` per changed translation unit, before (`origin/main`) and after.
- Unix backend on Windows: temporary, uncommitted instrumentation routes both SHA selectors to the non-BCrypt branches. Without OpenSSL it reproduces the Linux failure (RED); with engine OpenSSL forced on it runs the new code (GREEN). Instrumentation removed before the final build.
- Final supported MSVC build and the native suites covering every changed file.
