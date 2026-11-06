# UMG widget animation authoring — issue176 design

Date: 2026-10-08
Status: Implemented and independently source-reviewed; full rendering native/Python and actual SDK saved-widget author/cold-reload/playback acceptance passed on UE5.8.3. Prescribed cross-domain benchmark and merged delivery are in progress.
Issue: https://github.com/etelyatn/UnrealCortex/issues/176
Source baseline: 6506e03a21a8a2c6af1570b3d01e736947cd3040
Implementation owner: UnrealCortex / CortexUMG; existing Python umg_cmd router.
Delivery authority: User selected merged delivery, then requested subagent design review, patching, and continuation. Written architectural artifact approval gates remain.

## Intent and acceptance

Enable an agent to create the contents of a UMG animation using structured Cortex operations, not protected Python reflection or GUI automation. A saved animation must actually change its intended widget property when played. This is an enhancement: existing create_animation correctly creates an empty sequence.

| Criterion | Current evidence | Delivery proof |
| --- | --- | --- |
| Bind a Designer widget | Creation has no bindings; removal and inspection own serialized bindings | Actual routed binding creation, correlated UMG record and MovieScene possessable |
| Author float and color tracks, sections and keys | Native test fixtures construct float tracks; no public authoring command | Routed opacity and Image color creation/replacement; exact readback |
| Exact targeting and content guards | Existing binding selectors and canonical animation fingerprints | Wrong-case, ambiguity, stale guard and malformed payload refuse without mutation |
| Compile, save and survive cold reload | Explicit Blueprint compile and Core save exist | Fresh editor process reads the exact stored binding/property/ranges/keys/interpolation |
| Actual widget property changes | Not yet exercised | Real saved generated widget instance plays animation; inspect intermediate opacity/color and unchanged sibling |
| Existing reads/removal/rename remain consistent | Binding removal synchronizes representations; native rename propagates binding WidgetName | Author, rename, inspect, play, selectively remove; siblings and unaffected tracks preserved |

All rows are requirements. Implementation acceptance is now exercised: full rendering Cortex+ passed 1802/1802, nonlive Python passed 993/993, and actual stdio MCP author/cold phases passed 31/31 and 19/19. The fresh process retained exact GUID/property/ranges/keys/interpolation and digest, produced opacity/color midpoint effects with unchanged controls, and preserved rename/clear/removal semantics. Publication is not yet complete; exact evidence belongs in [the verification report](../verification/2026-10-08-issue176-umg-animation-authoring.md).

## Ownership and reuse

Keep new behavior in CortexUMG private operations. Reuse UWidgetBlueprint::Animations, UWidgetAnimation::AnimationBindings, UMovieScene possessables/bindings and native float/color tracks. Engine MovieScene and MovieSceneTracks are already private dependencies. No new module or cross-domain dependency.

Reuse list_animation_bindings as the read owner, remove_animation_binding as the whole-binding removal owner, and existing native widget rename propagation. Existing create_animation remains empty-sequence creation; widget_compose remains name/length animation creation. Do not add a standalone MCP tool or overload literal widget property setters.

Implementation anchors at baseline:
- Source/CortexUMG/Private/Operations/CortexUMGWidgetAnimationOps.cpp:22-66 creates empty animation; 161-565 inspects bindings and summary track counts.
- Source/CortexUMG/Private/Operations/CortexUMGAnimationBindingUtils.h:55-99 owns fingerprint and removal preflight.
- Source/CortexUMG/Private/Operations/CortexUMGAnimationBindingUtils.cpp:659-713 fingerprints bound track class/name and section channels; property identity needs explicit inclusion.
- Source/CortexUMG/Private/Operations/CortexUMGWidgetTreeOps.cpp:928-978 snapshots and renames binding records and possessables.
- Source/CortexUMG/Private/CortexUMGCommandHandler.cpp:268-289 publishes current animation operations.

UE5.8 WidgetAnimation.cpp:233-278 creates ordinary bindings using WidgetName and AnimationGuid; root-user-widget and slot bindings have different resolution semantics. New authoring supports ordinary named Designer widgets only. Existing inspection/removal support is not narrowed.

## Alternatives and decision

1. Separate guarded binding creation and a property-track setter: smallest update scope, easy reuse of existing deletion and inspection. Selected.
2. Whole-animation contents replacement: convenient bulk payload, but broadens rollback and preservation responsibility unnecessarily.
3. Individual add/remove commands for tracks, sections and keys: larger public surface with many intermediate states. Rejected.

Sections and keys are authored as one property-track value. This supports concrete animation authoring without general-purpose Sequencer editing.

## Commands

All requests are synchronous, dispatched on the Game Thread by the existing router. They accept strict JSON field types and reject unknown authoring fields. No implicit compile, save, reload, retry or target repair. Both new writes use direct uncached dispatch before generic MCP pagination; any supplied cursor/limit/offset field, including null, refuses before TCP forwarding. Detailed inspection also bypasses cached cursors and uses native binding offset/limit pagination.

### umg.ensure_animation_binding

Required fields:
- asset_path: Widget Blueprint path.
- animation_name: exact serialized animation object name.
- widget_name: exact case-sensitive Designer UObject name.
- expected_fingerprint: full animation fingerprint from list_animation_bindings.

Optional field: dry_run boolean, default true.

Load through the existing guarded package-loading owner. Locate exactly one animation and Designer widget. The target may be WidgetTree's root widget, but never infer bIsRootWidget=true: that flag refers to the UUserWidget instance, not the Designer root.

For an absent ordinary target binding, add one UMG binding and one MovieScene possessable with matching GUID and actual widget class. Return the full canonical selector {binding_guid, widget_name, slot_widget_name:"", is_root_widget:false}. For one existing healthy ordinary binding, return changed=false and its selector. Refuse duplicate target records, shared target GUIDs, dynamic records, absent possessables, wrong possessed class, parent/child possessables or conflicting malformed target relationships. Never merge or repair them.

Preview reports a proposed binding without persisting any generated GUID or dirtying objects. For creation, matched_selector is null in preview; planned_target identifies the widget. The applied response returns the actual selector and fresh fingerprint. An idempotent existing-binding preview returns its actual selector.

### umg.set_animation_property_track

Required fields:
- asset_path and animation_name.
- selector: the existing full binding tuple, using binding_guid (not animation_guid), widget_name, slot_widget_name, is_root_widget.
- property_path: exact reflected property path.
- track: object or explicit null.
- expected_fingerprint: full animation fingerprint.

Optional field: dry_run boolean, default true.

A track object contains type (float or color) and sections (nonempty array). Each section contains start_seconds, end_seconds and keys (nonempty array). Each key contains time_seconds, value, and interpolation (linear or constant). Color values are linear RGBA objects with exactly r/g/b/a numeric components. Missing fields, empty object/array/string, boolean numbers and explicit null numeric fields refuse; only track:null clears.

Example object:

```json
{
  "type": "float",
  "sections": [{
    "start_seconds": 0.0,
    "end_seconds": 0.1,
    "keys": [
      {"time_seconds": 0.0, "value": 0.0, "interpolation": "linear"},
      {"time_seconds": 0.1, "value": 1.0, "interpolation": "linear"}
    ]
  }]
}
```

Resolve native float or FLinearColor properties through typed reflection and native MovieScene property resolution. Do not require CPF_Interp: UE5.8 Image ColorAndOpacity is animatable through its native setter without that flag. New commands author native property tracks, never directly write property memory. Object traversal, array indexing, FSlateColor and custom property tracks are outside this contract. Support inherited RenderOpacity and Image ColorAndOpacity, using the engine's registered runtime accessor/native setter. Unknown or incompatible paths refuse before mutation. Existing track identity is binding GUID plus exact property path, not its editor display label.

Time is seconds at the API. Quantize with TickResolution.AsFrameTime(seconds).RoundToFrame(), checking finite nonnegative seconds, positive rate numerator/denominator, scaled-time overflow and representable int32 rounded frames before conversion. Reject duplicate quantized key frames, including repeated identical input times. Quantized sections are half-open evaluation ranges [S,E), with S<E, contained in the unchanged native playback range and nonoverlapping; adjacent [S,E) and [E,F) sections are valid. Stored keys may lie at E as interpolation control points, but E is not a guaranteed playback sample. Never extend ranges to include an endpoint: a sequence's last valid tick is playback upper bound minus one. Exact final-value playback checks must place the final key on that last evaluated tick. Stored frame numbers and normalized seconds are returned explicitly.

For color, create one native color track and section with four float channels and identical RGBA key times/interpolation. No implicit color-space conversion, clamping or cubic tangents.

Every authored section has explicit canonical evaluation state: Absolute blend, active and unlocked, row 0, overlap priority 0, RestoreState completion, zero pre/post roll, zero automatic/manual easing durations, no channel overrides and no weight channel. Float channels use the MovieScene tick resolution, unset default values and constant pre/post infinity extrapolation; keyed values are checked and normalized to native float precision. Linear and constant keys have canonical zero tangent/weight state. The track uses ordinary property evaluation and no nearest-section or preroll/postroll evaluation extensions. These fields are part of readback, guard coverage and idempotent equality, not inherited from a replaced section or engine serialization-version defaults.

The setter creates a missing property track, replaces exactly one supported existing property track, or clears exactly that track with null. Duplicate matching tracks, unsupported section/channel types, shared-GUID binding or malformed target relationships refuse. Clearing does not delete the binding/possessable: use existing remove_animation_binding explicitly for that. Retained property tracks, bindings, animations and playback range are unchanged. Identical requested authored contents return changed=false without transaction or Blueprint notification.

### Shared mutation response

Return asset_path, animation_name, dry_run, changed, would_change, fingerprint, matched_selector, property_path when applicable, reader_complete, and before/after summaries (umg_binding_count, movie_scene_binding_count, track_count). On preview, changed=false and fingerprint describes current state; after describes the projected result. On apply, fingerprint and after describe the verified live result. Return normalized authored_track for the setter: projected content on preview, live readback on apply, null after clear. No save_attempted/saved fields: these operations never save.

Before mutation, build the prospective normalized response and require at most 32,000 characters under the MCP formatter's indent=2, ASCII-escaping convention. Native validation must conservatively account for this same representation; a Python-only check after TCP mutation is insufficient. Setter limits are at most 8 sections and 64 logical keys across the complete track (up to 256 float-channel keys for color), plus the response budget; counts alone do not establish fit. Names/paths must fit Unreal's native name/path limits and the response budget. Oversized prospective content refuses before Modify with LIMIT_EXCEEDED and current_fingerprint.

The MCP formatter recognizes the new authoring commands before generic truncation. If an applied result unexpectedly exceeds 40,000 characters, retain asset_path, animation_name, dry_run, changed, would_change, fingerprint, matched_selector, property_path and before/after; omit only authored_track with authored_track_omitted=true, reader_complete=false and an explicit inspection referral. Never replace an applied outcome with a generic size refusal. The same bounded formatting applies to preview and native failure/recovery envelopes.

Error codes use existing CortexTypes.h constants: INVALID_FIELD for malformed JSON shape; WIDGET_NOT_FOUND/ANIMATION_NOT_FOUND for missing targets; ANIMATION_BINDING_NOT_FOUND/ANIMATION_BINDING_AMBIGUOUS/ANIMATION_BINDING_UNSUPPORTED for invalid binding relationships; INVALID_PROPERTY_PATH, PROPERTY_NOT_FOUND or TYPE_MISMATCH for path/type failure; INVALID_PROPERTY_VALUE for numeric/value failure; LIMIT_EXCEEDED for authoring bounds; STALE_PRECONDITION with current_fingerprint for guard mismatch; VERIFICATION_FAILED with rolled_back=true for failed apply/readback followed by proven restoration; DIRTY_EDITOR_STATE with rolled_back=false for unproven restoration. Native errors retain their error code/message through the MCP boundary instead of extracting an absent data object into a misleading empty success.

## Inspection and bounded transport

Extend list_animation_bindings with optional include_track_content boolean default false. Preserve existing summary fields and native binding pagination behavior. With opt-in content, each supported float/color track includes property_path, property_name, type, canonical evaluation state and sections. Each section reports exact native lower/upper range bound types and frames, normalized seconds, evaluation state and channels. Channels are identified as float or r/g/b/a and contain independently read arrays of {frame_number, time_seconds, value, interpolation}, plus defaults/extrapolation/tick resolution. Do not assume identical key times across the four channels of an existing color track. Unsupported tracks, overrides or uninterpretable channel/evaluation state remain visible with class/counts and content_supported=false plus diagnostics, not an empty supported track.

Detailed-read formatting runs before the generic nested-track truncator. Return either the complete native page with reader_complete=true, or a bounded {_error:"RESPONSE_TOO_LARGE", reader_complete:false, asset_path, animation_name, fingerprint, summary_counts, max_response_chars:40000} without detailed tracks. reader_complete describes completeness of returned records; pagination.is_complete separately describes whether every binding was returned. Whole-asset inspection retains orphan diagnostics. Binding pagination never silently cuts a selected binding's tracks/keys. Stop detailed extraction at the bounded response budget rather than allocating an unbounded key payload. Legacy summary-only formatting remains unchanged.

## Content guard

Reuse the animation fingerprint owner and verifier, not a second stale-state system. Add explicit property name/path and all authored property-track/section evaluation state affecting the new commands to canonical serialization. Verify float/color keys, interpolation, frame rates/ranges and exact target class/identity. Property identity-only edits must invalidate an earlier guard even without a new package dirty transition.

Cut over the existing umg.animation_binding signature to version 2 in both producer and verifier. Update all native callers, Python fixtures, generated discovery and documentation; version-1 guards refuse and callers must fetch a fresh read. No compatibility alias. Replace incidental version-only assertions with behavioral stale-guard coverage where appropriate. A mutation guard must reject unsupported/unreadable authored state rather than claim complete coverage; read diagnostics remain available. Clearing still needs a safe snapshot of the selected track and preservation evidence for retained content.

## Transaction, rollback and lifecycle

Validate the whole request and expected state before FScopedTransaction or Modify. Snapshot only the targeted records/objects plus owner notification state needed for restoration, and separately compute preservation evidence for retained data. Keep new UObject references GC-safe for the synchronous operation; use RF_Transactional and correct outers for all authored objects.

In one transaction call Modify on the Blueprint, animation, MovieScene and affected existing track/sections before mutation. Construct native possessables/tracks/sections/channels, verify exact serialized requested content and retained state, then notify Blueprint modification once. Failed apply/readback restores exact previous track membership, bindings, possessables and dirty state, verifies restoration, cancels the transaction, and returns failure. Transaction.Cancel alone is not restoration. Unproven restoration is a distinct hard failure, never reported as a successful rollback.

Use existing explicit blueprint.compile and core.save_asset after authoring. Save failures are outside these setters and cannot be disguised by authoring success. No dependent-package writes. Undo/redo must restore both representations and native channel data.

## Verification and delivery

Use generic task-owned Widget Blueprints under a scratch /Game/UI path, with Decoration and an unaffected sibling Image. Do not introduce game code or asset-specific plugin behavior.

Failing-first native tests must cover public command dispatch, ordinary/root-Designer binding semantics, float/color authoring readback, replacement/clear preservation, wrong-case and stale guard refusal, invalid values/time quantization, ambiguous records, preview, undo/redo and verified restoration. Use real native MovieScene data, not mock echo or source-text assertions. Retain zero-warning cleanup patterns and do not force-null reflection metadata during fixture GC.

Controller independently builds, runs the focused tests and full Cortex.UMG+ rendering suite serially, and exercises actual stdio MCP requests against the exact candidate. Cold persistence uses separate editor processes; playback uses a real generated widget and native UMG animation player with deterministic observation barriers derived from engine lifecycle, not polling until an expected value appears. Record selected project, process identity, source revision, exact readbacks and warnings/errors. No claim that a successful create response proves runtime property changes.

Update design + implementation docs, system references and a verification report; propagate new commands/contracts to toolkit following cortex-sync-toolkit. Keep parent gitlinks and unrelated user work uncommitted. Publish and merge only the reviewed/tested candidate with expected-head protection, then verify issue closure and preserved workspace state. UE5.8 is locally available; UE5.6/5.7 runtime compatibility is unverified and must be stated, not claimed.

Discovery updates include native Execute/GetSupportedCommands registration, live/profile operation-schema coverage, capabilities fixture regeneration, generated fallback updates and toolkit command examples. Schema fields use binding_guid consistently with the actual existing parser; do not copy the older system document's animation_guid shorthand. Neither new command opts into rollback-enabled Core batches without an implemented batch recovery contract.

Boundary acceptance includes real registered-router scenarios for a valid cached cursor from another read supplied to either writer, null/boolean/fractional pagination fields, an oversized mutation response, a many-track detailed page and a single oversized detailed binding. Verify refusal or explicit incomplete/outcome envelopes, never mock echoes. Native acceptance also includes adjacent sections, a stored key at the excluded playback upper bound, last-valid-tick playback and replacement of matching keys with noncanonical blend/easing/default state.

## Non-goals

No runtime playback MCP command, root-user-widget/slot/dynamic-binding authoring, event/object/bool/custom tracks, arbitrary Sequencer automation, cubic interpolation/tangent editing, automatic animation-range changes, implicit compile/save, property-binding changes, generalized animation replacement, or widget_compose schema expansion.

## Elicitation findings — native and public-contract review

The user authorized independent subagent review, correction of supported findings and continuation. Two read-only review lenses assessed the same specification: native UE architecture and MCP/public contract. Neither ran builds/tests, edited source or set aside any issue acceptance criterion. The controller verified consequential citations before accepting corrections.

| Finding | Confirmed evidence | Specification correction |
| --- | --- | --- |
| Applied outcomes could be lost at the 40k boundary | MCP/src/cortex_mcp/response.py:111-184,257-282; controller formatter smoke returned only _error/_size/_suggestion, with changed/fingerprint absent | Pre-mutation 32k budget and dedicated outcome/error preservation |
| Detailed reads could silently omit tracks | response.py:188-216; controller six-track smoke returned four tracks, _tracks_truncated=true and pagination.is_complete=true | Complete-or-explicit-incomplete detailed-read branch before generic truncation |
| Cached pagination could intercept authoring | MCP/src/cortex_mcp/tools/routers.py:268-287 routes existing animation commands before cursor dispatch, but new names would otherwise fall through | Direct uncached dispatch and rejection of any supplied pagination field |
| Endpoint contract contradicted native playback | UE5.8 MovieScenePlaybackManager.cpp:353-356 returns sequence end minus one tick | Checked RoundToFrame, half-open evaluation ranges, stored end control keys and last-valid-tick runtime assertions |
| Identical keys can hide different evaluation behavior | UE5.8 MovieSceneFloatSection.cpp:20-30 and MovieSceneColorSection.cpp:107-116 choose version-dependent completion; native test serializer preserves evaluation/default/extrapolation state | Explicit canonical section/channel defaults included in readback, guard and idempotence |

Source checks also confirmed Image ColorAndOpacity is FLinearColor with a native setter but no CPF_Interp flag (UE5.8 Image.h:45-48), and RenderOpacity has a registered runtime accessor (MovieSceneUMGComponentTypes.cpp:168). The existing digest includes all Designer widget names/classes/hierarchy before animation bindings (CortexUMGAnimationBindingUtils.cpp:524-550), so creation targets already participate in its tree-state guard.

The controller's formatter smoke used the installed environment with uv run --no-sync. The first ordinary uv run attempted environment synchronization and encountered a locked cortex-mcp.exe; no process was terminated. These are response-boundary observations using synthetic data, not live MCP/native-animation acceptance.

Both original reviewers re-read the corrected contracts and returned no unresolved findings within their assigned scopes. The design is ready for user written-spec review, not a verified implementation. Production source, native tests, cold persistence and widget playback remain unchanged/unexercised.

## Simplicity check

- [x] Think before acting — issue acceptance and existing animation/rename/fingerprint owners inspected at the baseline; runtime evidence is explicitly unexercised.
- [x] Simplicity first — two routed commands and opt-in existing inspection; native MovieScene owners and existing removal reused.
- [x] Surgical changes — ordinary widget float/color authoring only; unrelated domains, composite creation and parent gitlinks excluded.
- [x] Goal-driven execution — five supported design findings corrected and rechecked by both reviewers; controller source checks and formatter smoke recorded above. User approved the written specification. Implementation/runtime acceptance remains explicitly unverified.

## Compiler compatibility correction — PR178

JSON object iteration uses `const auto&` so UE 5.8's shared-string keys do not create temporary FString-keyed map pairs. Consumed keys use the existing `CortexEngineCompat::JsonKeyToString` version adapter; inspection field names, nested values and conservative response budgets remain unchanged. No command, schema, durable format or lifecycle change.

Independent compiler and runtime evidence: [PR178 verification](../verification/2026-10-13-pr178.md). Linux build/test results remain attributed to contributor @Boshchuk.
