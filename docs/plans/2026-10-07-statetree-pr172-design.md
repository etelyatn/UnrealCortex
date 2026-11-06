# PR 172: StateTree multi-root selection and stored inspection — local candidate verified

## Intent and source

Adopt both game-agnostic contributions from mavka-games in [PR172](https://github.com/etelyatn/UnrealCortex/pull/172), correcting the review findings rather than replacing sound work. Contributor commits `6a87b744dcbd394c297937c7f63468680a5fcaf2` and `b23fbb510659402946db98edf6ca5e736b538893` are retained unchanged. Integration base: `70ece02d9fa5e0145501b1f14412545780d898f7`; completed shared-selector task: `00297cf3bb4d5ccd3ae41b1a21e41ed047a62fcb`; frozen tested native/MCP source: `f41162ffd733b2223d364ee6c8ced5cbc5b26294`. Maintainer adaptation supplies strict validation, page-before-reflection, lossless numeric/container completion, traversal/diagnostic corrections and test-fixture ownership guards. User authorized work and a final very-minor version bump. No project-specific task replacement, new MCP tool, or mutation API. This is a locally verified candidate, not a published or merged release; the final descriptor bump/review remains controller-owned.

## Shared state enumeration

The existing CortexST private helper owns `CollectAllStates(Context, OutStates, bIncludeSelectorFields = true)` and single-root `CollectStates`. Traverse each non-null subtree in stored order, children depth-first, using one visited-state set across roots seeded from existing output. Repeated/cyclic child references are visited once with first ownership/order preserved; traversal never rewrites native Parent or Children. Normal dump_tree, get_state, mutation selectors, and transition selectors agree on the addressable state set. GUID/path selectors use the default metadata mode; stored inspection passes false to avoid building off-page selector strings or walking a malformed native Parent chain. Preserve GUID identity, existing path ambiguity errors, and first valid root defaults. A null first slot does not reject explicit later-root selectors. The operation-local duplicate enumeration helper is removed. Existing root deletion/reparenting restrictions remain unchanged.

## Stored inspection

Retain opt-in inspect_instances on dump_tree, existing section names, stored value provenance, partial/issues reporting, and identity-only object references. Inspection never compiles, saves, or edits the asset. Keep ordinary dump_tree compatible apart from fixing omitted subtree roots.

Parse present fields strictly: inspect_instances is boolean; inspect_section is one of root/states/nodes/bindings; offsets/counts are finite integral JSON numbers with existing bounds. Omitted fields retain documented defaults. Paging controls without an enabled inspection and section must reject rather than silently disappear. Validate malformed input before expensive reflection.

Enumerate section entries as references/descriptors, count them, select the requested range, then serialize that range. Root is one entry; states omit separately paged node bodies; nodes include evaluator/global tasks, state task/single-task/conditions/considerations and transition conditions with owner identity; bindings contain stored binding entries. A valid empty terminal page reports total, offset, returned_count=0, has_more=false. Keep an explicit unpaged inspection path; do not misrepresent entry paging as a byte-size bound.

Reuse shared Core leaf serializer policies, with local StateTree stored-compound traversal for fixed arrays, structs, arrays, sets/maps and optionals. Compound branches accumulate and append their own partial/issues metadata: flattened map-key diagnostic prefixes are not ownership boundaries because keys may contain dots/brackets. Unsupported values retain honest per-field diagnostics, not invented defaults or silently lost elements. Depth limits, identity-only object references and non-deterministic set-order diagnostics remain explicit.

Source-confirmed numeric completion: actual uint32 stored fields must serialize exactly, including values above INT32_MAX. GUID components preserve their native 32-bit identity; signed reflected components are valid and must not be rewritten solely to force unsigned display. Signed 64-bit stored integers outside the interoperable JSON safe-integer range ±9007199254740991 serialize as exact decimal strings; safe-range values remain numbers. This is local to the new StateTree inspection payload, not a change to shared Core serializer contracts. Reflected cpp_type identifies the original field type. Nested fixed arrays retain every element, including when reached through containers/optionals.

## Native acceptance fixture and compatibility

The private Tests `UCortexSTStoredInspectTestUtility` exposes only `PrepareTopology`, `PopulateStoredInstances`, and `CaptureSnapshot` for trusted local automation of the genuine regression fixture. It adds no production route. Admission requires Game Thread execution, exact case-sensitive loaded object identity under `/Game/Temp/CortexMCPTest/PR172_`, a top-level unsaved StateTree with no package file, EditorData Outer equal to that tree, and cycle-safe ownership checks of all reachable states, Parent references and node instance/runtime objects before Context assignment or mutation. Five alias regressions cover foreign EditorData at blank/prepared stages, foreign first/later roots and a foreign child; all three methods refuse and both assets remain unchanged.

Independent native snapshots serialize complete stored objects/compiled storage with bytes-only, no-delta object writers, not inspection JSON, and never call compile, PreSave, SavePackage or reference-modifying archive flags. Clean/dirty fixtures retain exact storage, compiled hash/signature and dirty state across reads. Python cannot reflect plain inaccessible EditorData storage, so this guarded fixture bridge is used rather than a mock or production escape route.

UE 5.6 lacks definition-node identity and execution-runtime storage members: inspection reports `definition_id_available=false` and runtime `engine_member_available=false`, rather than inventing values. UE 5.7+ uses actual members; fixture initialization/name/outer-enumeration has UE 5.8 guards. UE 5.6/5.7 were source-reviewed only, not locally built or run.

## Acceptance

Native regressions cover later-root GUID/path reads and writes, null root slots, cross-root ambiguous paths, exact stored values, page boundaries, malformed types/ranges, and read-only package/asset state. Real MCP smoke captures a multi-root fixture through every section and compares paged stored values with known fixture values without compile/save side effects. Build supported local UE; inspect version-sensitive engine fields against the plugin's UE 5.6+ policy and report any unavailable compatibility verification explicitly.

Synchronize command/toolkit guidance and project documentation as applicable, independently review the exact integrated candidate, and preserve contributor authorship. At the end bump UnrealCortex.uplugin VersionName from 0.3.5 to 0.3.6 and Version from 17 to 18, unless fresh repository state establishes a newer version, then increment that patch/revision once. Verify descriptor consistency and review the final version delta before publication.

## Observed local acceptance and durable rulings

- Controller-observed supported UE 5.8.3 build succeeded with zero compiler warnings; final native StateTree suite passed 52/52 with no raw Warning:/Error: lines. Full rendering `Cortex+` passed 1738/1738; its explicit expected-negative diagnostics are reported separately, not described as a raw-zero-error run.
- Actual stdio MCP passed single-factory-root unpaged inspection, null-first-slot/later-root GUID/path/default selection and guarded rename, then every populated section with count=1 and exact terminal metadata (root=1, states=3, nodes=4, bindings=2), known native typed values, real binding identities, malformed-input refusal and independent unchanged snapshots/fingerprints/files.
- Retain the existing shared MCP 40k formatter. The populated typed unpaged MCP response is deliberately rejected as `RESPONSE_TOO_LARGE` (reported compact `_size=60682`); full unpaged equivalence is proven by read-only TCP at the same verified editor. Entry count bounds work, not bytes; do not add byte-budget APIs, shrink the fixture or claim large unpaged MCP success.
- Isolated non-e2e/non-scenario/non-stress Python acceptance passed 970 tests, 265 deselected. Initial user-temp-root access failures are fixture-environment setup failures, not production failures.
- Cleanup uses function-local Python wrappers, targeted `unreal.purge_object_references` for only the owned tree including inners, and the existing fresh-fingerprint guarded deletion. No global UObject GC or new deletion fallback. Task-owned editor shutdown preserves unrelated editors.
- Source-checked reviews found and corrected diagnostic branch ownership, repeated/cyclic traversal, version-specific fixture APIs and foreign fixture ownership. Controller owns runtime evidence; workers do not build/test. Fresh sessions replaced stale implementers on the user's explicit request.

Detailed observed RED→GREEN evidence and limitations are retained in the [owning-plugin verification report](../verification/2026-10-07-pr172.md).

## Workspace and authority

Normal branch inside Plugins/UnrealCortex; no worktree/reset/clean/stash. Preserve parent plugin/toolkit gitlink modifications and any unrelated user work. Do not force-push or delete contributor branches. Publishing or merging must use exact-head protection and an explicit authority determination; planning alone is not publication authority.

## Final closeout correction

Final reviewed native/MCP candidate: `33bdfdff4b3f841f90403ace5204edd5a52a506c`. It includes guarded depth-before-unwrapping and definitionless single-task body preservation in paged nodes; isolated regressions preserve the normative fixture manifest. Both final source findings are addressed. Earlier StateTree53/53, full1739/1739 and actual stdio `Saved/PR172DepthFinalMcpAcceptance.json` are historical depth-checkpoint evidence. See the verification report for final rebuilt acceptance, publication and the user's explicit exception for two unchanged animation warnings. No byte-budget API or successful large-unpaged MCP claim.
