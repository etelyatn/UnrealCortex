# PR #136: complete portable build corrections

Contributor: Boshchuk. Submitted head3daecfe7018e0a1de4200c9e6cff5124d52c2249, old base c941bccb. Maintainer integration base77238c195664ca040fd02b1fc46b1378208ac1f2 after separately delivered #134/#135 and declared prerequisite #144.

| Behavior | Value | Existing design owner | Route / acceptance |
|---|---|---|---|
| Actual FJsonObject Values key type | UE5.8 shared-string entries must not bind a temporary explicit FString pair | CortexEngineCompat helper and existing deduced loops | Retain submitted corrections; cover new current-main loops after compiler reproduction; real Clang compilation plus existing consumer regression and runtime reads/writes |
| LiveCoding platform availability | Windows-only dependency/header must not enter unsupported builds | Target.bWithLiveCoding / WITH_LIVE_CODING | Retain submitted gate; build disabled and enabled Windows configurations, source-review target policy; no local Linux claim |

Current main added RewireInventory forwarding and CountJsonNumbers/EncodedResponseChars loops since the submitted base. These are consumers of the same correction, not optional scope. Preserve their bodies and change only the entry binding type after observed compiler failure. Leave explicit FString-keyed local buffers/maps unchanged.

No new compatibility API, validation, retries, telemetry or fallback. No changes to commands, response schemas, lifetime, transactions, explicit save or module layering. Existing system/toolkit descriptions remain accurate. Regression fixtures use existing suites; compiler portability is checked by compiling actual source, not by permanent source-text tests.

Contributor Linux results are attributed, including stacked verification. Local engine is Windows UE5.8.3 with no Linux SDK/editor. Local Clang provides direct evidence for shared-string diagnostics and Windows NoLiveCoding exercises the disabled branch; neither substitutes for a Linux launch/build claim.
