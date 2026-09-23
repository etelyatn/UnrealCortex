# Coordinated Minor Release Design

## Goal

Version the existing UnrealCortex and Cortex Toolkit candidates for their next minor releases, preserve exact cross-repository candidate links, and merge the requested PRs without losing the separate stacked PR #131.

## Current state

- UnrealCortex PR #132 is open on `feat/100-typed-blueprint-authoring`, based on `main`, at candidate `fa0f8fa368991b884e7a80e9d8d4dc675d333d3c`. Its plugin manifest reports `VersionName: 0.1.17` and integer `Version: 10`.
- Cortex Toolkit PR #58 is open on `feat/100-typed-blueprint-authoring`, based on `main`, at candidate `2b044fc168884bb04d231e25e7e7835b56826bf7`. Toolkit package metadata reports `0.9.1`.
- CortexSandbox PR #103 is open on `feat/100-typed-blueprint-authoring`, based on `main`, at candidate `a7a7a13e08b2539b9428ec7bdd74cbd3a6cc8757`; its submodule gitlinks point to the two candidates above.
- UnrealCortex PR #131 (`feat/102-schema-blueprint-catalog`) is open and based on PR #132's feature branch. It is not in the requested merge set. After #132 merges, retarget #131 to `main` and keep it open; do not merge it or close its linked UnrealCortex issue #102.
- CortexSandbox issue #102 remains open for the large-prune MCP response-bound work. That response-bound guard is not implemented. The earlier full connected-editor E2E run was red; the release descriptions and verification report must preserve this limitation.

## Version decisions

- UnrealCortex: `VersionName` `0.1.17` → `0.2.0`; integer plugin `Version` `10` → `11`.
- Cortex Toolkit: `0.9.1` → `0.10.0` in `package.json`, `.claude-plugin/marketplace.json` (both version entries), `.claude-plugin/plugin.json`, `.codex-plugin/plugin.json`, and `.cursor-plugin/plugin.json`.
- OpenCode has no separate version manifest; its installation consumes the package version.
- No external Claude Code, Codex, Cursor, or OpenCode application-version constraints are declared by this release. Do not invent or change external harness minimum versions.
- Do not create Git tags, GitHub Releases, or additional PRs; this request is limited to repository version metadata and the existing PRs.

## Integration approach

Put the metadata changes on the existing feature branches: a version-only commit on PR #132, a version-only commit on PR #58, then update the Sandbox submodule gitlinks and candidate descriptions on PR #103. Update the three PR bodies to reflect the final exact heads and keep the known limitations explicit.

Merge with GitHub merge commits in dependency order: #132, #58, then #103. All three repositories allow merge commits. This retains the feature-branch commit ancestry required to retarget stacked PR #131 to `main` without making PR #132's changes appear again in #131. After merging #132, retarget #131 to `main`; leave it open and do not merge it. No issue state, review thread, or approval other than the linked effects of the selected PR merges is to be changed.

## Verification

- Parse every edited JSON manifest and assert the exact version values and equality across all toolkit distribution manifests. No Unreal build is needed for metadata-only JSON changes; retain the existing source-candidate build evidence in PR #132's description.
- Run the toolkit unit suite; do not re-run the known-broad failing E2E suite for version-only metadata.
- Before each merge, verify the expected PR head, base, open state, mergeability, and clean merge state. Merge #132, retarget and verify #131 is open on `main`, then merge #58 and #103. Afterward verify the three requested PRs are merged, #131 remains open against `main`, submodule pointers match the merged candidate commits, and CortexSandbox #102 remains open.
- Do not claim the large-prune response-bound behavior or broad E2E suite is fixed or passing.

## Alternatives considered

1. **Version the existing PRs (selected):** minimal coordination and each PR body identifies the exact versioned candidate that will be merged.
2. **Merge feature PRs first, then open version-only PRs:** cleaner separation of release metadata, but creates two additional PRs and delays the requested release version changes.
