"""Response size guard for MCP tool results."""

import json
import logging

logger = logging.getLogger(__name__)

MAX_RESPONSE_CHARS = 40_000
_MIN_LIST_SIZE = 10


def compact_json(value) -> str:
    """The single emitted and budget-counted JSON representation.

    Compact separators and no ASCII escaping: this is the text a caller receives, so
    every size decision must be measured against it, not against a padded/escaped form.
    Durable artifacts, hashes and protocol input serialization keep their own encoding.
    """
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


# Animation authoring writes are bounded by a dedicated formatter: an applied outcome is never
# replaced by a generic size refusal, and cached-array pagination never applies to them.
ANIMATION_AUTHORING_COMMANDS = frozenset({
    "ensure_animation_binding",
    "set_animation_property_track",
})

_AUTHORED_TRACK_REFERRAL = (
    "Detailed authored track omitted because the response exceeds "
    f"{MAX_RESPONSE_CHARS} characters. Use umg.list_animation_bindings with "
    "include_track_content=true to inspect the stored animation content."
)

# Reads whose owner exposes an explicit selector are complete-or-refuse: an oversized page is
# refused with the applicable selector instead of being silently shortened by the generic array
# truncator, and the refusal never advertises a limit the read cannot consume. The selector
# itself is implemented in the native owners (Tasks 5/10); until then the refusal is the whole
# truthful answer. Reads that fit the budget are never policed.
_PROPERTIES_DISCOVERY_SELECTOR = (
    "Select only the needed 'properties', or use include_values=false / discovery_limit for "
    "metadata discovery."
)
_CATALOG_SECTION_SELECTOR = (
    "Request only the needed 'sections', and keep include_asset_paths=false for the default view."
)
_REFLECT_DETAIL_SELECTOR = "Request detail=summary, or narrow the class you inspect."
_REFLECT_CONTEXT_SELECTOR = (
    "Request detail=summary, or read the parent properties only instead of the whole context."
)
# Both extended material reads can also refuse; the supported narrowing is their own caller-side
# selectors, never a limit the read does not consume.
_MATERIAL_INPUT_STATE_SELECTOR = (
    "Select fewer 'properties', omit include_input_state, or read the root state separately "
    "through material.get_material."
)
_MATERIAL_CONNECTIONS_SELECTOR = (
    "Omit include_input_state, or read the root state separately through material.get_material."
)
_COMPLETE_OR_REFUSE_POLICIES: dict[str, str] = {
    "data.get_data_asset": (
        "Select the needed members with 'fields' instead of reading the whole DataAsset."
    ),
    "material.get_node": (
        "Request only the needed 'properties' of the node, or read the node identity alone."
    ),
    "material.get_material": _MATERIAL_INPUT_STATE_SELECTOR,
    "material.list_connections": _MATERIAL_CONNECTIONS_SELECTOR,
    "umg.get_tree": (
        "Bound the traversal with 'depth' or 'max_nodes', use names_only=true for a lean "
        "hierarchy, or inspect one widget with umg.get_widget."
    ),
    "graph.get_subgraph": (
        "Select fewer 'node_ids' (a smaller subgraph never pages the whole edge topology), or "
        "pass include_edges=false to read the node identities alone."
    ),
    "blueprint.get_class_defaults": _PROPERTIES_DISCOVERY_SELECTOR,
    "blueprint.list_settable_defaults": _PROPERTIES_DISCOVERY_SELECTOR,
    "blueprint.list_inherited_properties": _PROPERTIES_DISCOVERY_SELECTOR,
    "blueprint.analyze_for_migration": (
        "Request only the needed 'sections' (e.g. variables, functions, graphs, complexity_metrics) "
        "instead of the whole migration analysis."
    ),
    "blueprint.graph_metrics": (
        "graph_metrics has no row selector: use analyze_for_migration with "
        "sections=['complexity_metrics'] for a bounded summary, or split the oversized Blueprint."
    ),
    "get_capabilities": (
        "Request detail=names for the per-operation schema index, or inspect one operation schema."
    ),
    "get_data_catalog": _CATALOG_SECTION_SELECTOR,
    "data.get_data_catalog": _CATALOG_SECTION_SELECTOR,
    "query_class_detail": _REFLECT_DETAIL_SELECTOR,
    "reflect.class_detail": _REFLECT_DETAIL_SELECTOR,
    "query_class_context": _REFLECT_CONTEXT_SELECTOR,
    "reflect.class_context": _REFLECT_CONTEXT_SELECTOR,
    "batch_query": (
        "Pass 'limit' to page the step results from one execution (follow _pagination.next_cursor), "
        "or split the batch into smaller batches."
    ),
    "reflect.find_duplicate_graphs": (
        "Narrow 'path_prefixes' to fewer content folders, raise 'min_nodes', or raise 'similarity' "
        "so fewer groups are reported."
    ),
    "reflect.find_name_lookups": (
        "Narrow 'path_prefixes' to fewer content folders, or pass 'symbols' to report only the "
        "names you are checking."
    ),
    "reflect.find_broken_config_paths": (
        "find_broken_config_paths has no row selector: fix the broken config paths reported so far "
        "and re-run, or use reflect.find_usages / reflect.get_referencers (include_config) on the "
        "specific assets in question."
    ),
}


def _find_largest_list(data: dict) -> str | None:
    """Find the key of the largest list with _MIN_LIST_SIZE+ items in data."""
    best_key = None
    best_len = 0
    for key, value in data.items():
        if isinstance(value, list) and len(value) >= _MIN_LIST_SIZE and len(value) > best_len:
            best_len = len(value)
            best_key = key
    return best_key


def _bound_nested_tracks(data: dict, max_tracks: int) -> dict:
    """If data has 'bindings', bound nested 'tracks' array within each binding."""
    if "bindings" not in data or not isinstance(data["bindings"], list):
        return data
    new_bindings = []
    any_modified = False
    for b in data["bindings"]:
        if isinstance(b, dict) and isinstance(b.get("tracks"), list) and len(b["tracks"]) > max_tracks:
            any_modified = True
            b_copy = dict(b)
            orig_tracks = b["tracks"]
            b_copy["tracks"] = orig_tracks[:max_tracks]
            b_copy["_tracks_truncated"] = True
            b_copy["_tracks_total"] = len(orig_tracks)
            b_copy["_tracks_returned"] = max_tracks
            new_bindings.append(b_copy)
        else:
            new_bindings.append(b)
    if not any_modified:
        return data
    new_data = dict(data)
    new_data["bindings"] = new_bindings
    return new_data


def _bound_authoring_response(data: dict) -> str:
    """Bound an animation authoring result without losing its outcome.

    Native authoring envelopes are bounded before mutation. If detailed authored_track
    content exceeds the transport limit, omit that content explicitly, never outcome,
    current guard or error fields. Arbitrarily oversized error text cannot be made
    bounded while also preserving it verbatim; native validation must bound its source.
    """
    text = compact_json(data)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text

    bounded = dict(data)
    omitted_track = "authored_track" in bounded
    if omitted_track:
        del bounded["authored_track"]
    bounded["reader_complete"] = False
    if omitted_track:
        bounded["authored_track_omitted"] = True
    bounded["_authored_track_instructions"] = _AUTHORED_TRACK_REFERRAL
    return compact_json(bounded)


# Names a refusal field owns: context from the request or the native echo can never overwrite them.
_RESERVED_REFUSAL_KEYS = frozenset({
    "success",
    "reader_complete",
    "qualified_command",
    "_error",
    "_message",
    "_suggestion",
    "_native_page_omitted",
    "_omitted_context",
})


# Native diagnostics that accompany an incomplete read (partial/issues/warnings). They are context,
# not the value payload: carried when they fit, and reported by count plus an explicit omission when
# they cannot, so a refusal never silently loses why a read was incomplete.
# Continuation fields that advertise rows the caller has not received. A refusal carries no rows, so
# republishing one would let a client advance on it and skip the rejected page entirely.
_ADVANCING_CONTINUATION_KEYS = ("next_offset", "next_cursor")

_DIAGNOSTIC_ARRAY_KEYS = ("issues", "warnings", "diagnostics")


def _selector_context(data: dict, request_params: dict | None) -> dict:
    """Selector/window context for a refusal: native scalars win, request selectors fill gaps.

    Arrays and nested objects are the incomplete payload itself and are never echoed. A native
    value always wins a collision, because the owner emits the window it actually applied (for
    example `level.list_actors` clamps the offset and cap and reports the applied values), so the
    refusal must never advertise an unconsumed request window. The caller's selectors are still
    carried when the native response does not echo them (for example `umg.get_tree` has no asset
    path), and request-side telemetry keys are ignored.
    """
    context: dict = {}
    if isinstance(request_params, dict):
        for key, value in request_params.items():
            if key in _RESERVED_REFUSAL_KEYS or key.startswith("_") or isinstance(value, (list, dict)):
                continue
            context[key] = value
    for key, value in data.items():
        if key in _RESERVED_REFUSAL_KEYS or key.startswith("_") or isinstance(value, (list, dict)):
            continue
        context[key] = value
    for key in _DIAGNOSTIC_ARRAY_KEYS:
        value = data.get(key)
        if isinstance(value, list):
            context[f"{key}_count"] = len(value)
            context[key] = value
    return context


def _bounded_refusal(payload: dict, context: dict) -> str:
    """Attach as much selector context as fits, dropping whole oversized fields explicitly.

    A field is never truncated: when the refusal would exceed the budget the largest remaining
    context value is dropped and its key is listed in `_omitted_context`, so the omission is
    reported instead of hidden. The fixed refusal fields are never dropped.
    """
    remaining = dict(context)
    omitted: list[str] = []
    while True:
        trial = dict(payload)
        trial.update(remaining)
        if omitted:
            trial["_omitted_context"] = sorted(omitted)
        if len(compact_json(trial)) <= MAX_RESPONSE_CHARS:
            return compact_json(trial)
        if not remaining:
            # Fixed refusal fields are bounded, so this keeps the guarantee unconditional.
            trial.pop("_omitted_context", None)
            return compact_json(trial)
        largest = max(remaining, key=lambda key: (len(compact_json({key: remaining[key]})), key))
        del remaining[largest]
        omitted.append(largest)


def _refuse_oversized_read(
    data: dict,
    label: str,
    suggestion: str,
    request_params: dict | None = None,
) -> str:
    """Refuse an oversized read that owns an explicit selector.

    The applicable selector is named instead of shortening the payload: a shortened array could be
    mistaken for a complete read, and the selector already exists to make the read affordable.
    """
    payload = {
        "_error": "RESPONSE_TOO_LARGE",
        "_message": (
            f"{label} response exceeds {MAX_RESPONSE_CHARS} characters; "
            "the complete read was refused."
        ),
        "reader_complete": False,
        "_suggestion": suggestion,
    }
    return _bounded_refusal(payload, _selector_context(data, request_params))


def format_native_page(data: dict, qualified_command: str, params: dict) -> str:
    """Complete-or-refuse formatting for a native-owned page.

    A native page carries native totals/completeness and a continuation computed for the whole
    page, so it is never shortened here: the complete page is returned, or an explicitly
    incomplete refusal preserves the window/selector context, exposes no advancing continuation
    and names a smaller limit or field selection. Even a single oversized record refuses. The
    context is bounded: an oversized selector/echo value is dropped whole and named in
    `_omitted_context`, and the native window scalars win any collision with the request.
    """
    text = compact_json(data)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text

    payload = {
        "_error": "RESPONSE_TOO_LARGE",
        "_message": (
            f"{qualified_command} native page exceeds {MAX_RESPONSE_CHARS} characters; "
            "the complete native page was not returned."
        ),
        "reader_complete": False,
        "_native_page_omitted": True,
        "qualified_command": qualified_command,
        "_suggestion": (
            "Re-request with a smaller limit or select fewer fields; the response guard never "
            "shortens a native page because the continuation was computed for the full page."
        ),
    }
    context = _selector_context(data, params)
    # The refusal returns no rows, so it must not advertise progress through the rows it omitted.
    for key in _ADVANCING_CONTINUATION_KEYS:
        context.pop(key, None)
    return _bounded_refusal(payload, context)


def _bounded_detail_oversize(data: dict) -> str:
    """Explicit incomplete envelope for oversized detailed inspection.

    Detailed tracks are never silently cut: either the complete native page is returned or this
    summary envelope, so a partial page can never claim completeness.
    """
    counts = data.get("summary_counts")
    if not isinstance(counts, dict):
        counts = {
            key: data[key]
            for key in ("umg_binding_count", "movie_scene_binding_count", "track_count")
            if key in data
        }
    payload = {
        "_error": "RESPONSE_TOO_LARGE",
        "_message": (
            "Detailed animation binding content exceeds the response limit. "
            "Re-request with a smaller limit, or omit include_track_content for the summary read."
        ),
        "reader_complete": False,
        "asset_path": data.get("asset_path"),
        "animation_name": data.get("animation_name"),
        "fingerprint": data.get("fingerprint"),
        "summary_counts": counts,
        "max_response_chars": MAX_RESPONSE_CHARS,
    }
    return compact_json(payload)


def _format_detailed_read(data: dict) -> str:
    """Complete-or-explicit-incomplete formatting for include_track_content reads."""
    if data.get("reader_complete") is False:
        # Native already stopped detail extraction at its own budget.
        return _bounded_detail_oversize(data)
    complete = dict(data)
    complete["reader_complete"] = True
    text = compact_json(complete)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text
    return _bounded_detail_oversize(data)


def format_response(
    data: dict,
    tool_name: str,
    *,
    command: str | None = None,
    detail: bool = False,
    qualified_command: str | None = None,
    request_params: dict | None = None,
) -> str:
    """Serialize data to JSON, truncating array results if over size limit.

    If the response exceeds MAX_RESPONSE_CHARS:
    1. For mutation results with remaining_bindings, essential outcome fields
       are always preserved, and remaining_bindings is truncated with dedicated metadata.
    2. Reads that own an explicit selector are complete-or-refuse, naming that selector.
    3. For binding queries, nested track details are bounded first so canonical selectors
       remain retrievable under the 40k limit.
    4. Diagnostics are truncated separately with independent metadata without corrupting
       binding pagination.
    5. Paginated arrays are truncated with reconciled pagination metadata guaranteeing
       forward progress.

    Args:
        data: The response data dict.
        tool_name: Name of the tool for error messages, and the policy key for standalone reads
            that have no routed qualified command.
        command: The routed command name, used for dedicated authoring formatting.
        detail: True for include_track_content reads, which are formatted before the
            generic nested-track truncator as complete-or-explicit-incomplete.
        qualified_command: The exact routed read command (e.g. ``data.get_data_asset``), used to
            resolve the complete-or-refuse selector policy.
        request_params: The caller's selector/window, kept as context for refusals and errors.

    Returns:
        JSON string, guaranteed under MAX_RESPONSE_CHARS.
    """
    if command in ANIMATION_AUTHORING_COMMANDS:
        return _bound_authoring_response(data)
    if detail:
        return _format_detailed_read(data)
    text = compact_json(data)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text
    # Asset-owned serialized identities cannot be silently cut or treated as empty.
    state = data.get("property_binding_state")
    if isinstance(state, dict):
        bounded_state = {
            key: state[key]
            for key in ("scope", "total", "fingerprint")
            if key in state
        }
        bounded_state["reader_complete"] = False
        return compact_json({
            "_error": "RESPONSE_TOO_LARGE",
            "_message": (
                "Serialized binding inspection exceeds the response limit. "
                "Existing widgets can be inspected individually with get_widget; "
                "oversized orphan records cannot be retrieved through widget-scoped inspection."
            ),
            "property_binding_state": bounded_state,
        })

    fingerprint = data.get("fingerprint")
    signature = fingerprint.get("domain_signature") if isinstance(fingerprint, dict) else None
    if (
        isinstance(signature, dict)
        and signature.get("scope") == "umg.property_binding"
        and "changed" in data
    ):
        # Mutation already happened. Preserve its outcome even if an identity is huge.
        bounded = dict(data)
        bounded["reader_complete"] = False
        bounded["_binding_identity_truncated"] = True
        bounded["_message"] = "Mutation outcome retained; an oversized serialized identity was omitted."
        for key in ("before_binding", "binding"):
            if len(compact_json(bounded)) <= MAX_RESPONSE_CHARS:
                break
            if isinstance(bounded.get(key), dict):
                del bounded[key]
                bounded[f"_{key}_omitted"] = True
        return compact_json(bounded)


    # 1. Special handling for UMG animation binding mutation results:
    # Essential outcome fields must never be replaced with generic errors.
    if "remaining_bindings" in data and isinstance(data["remaining_bindings"], list):
        bindings = data["remaining_bindings"]
        original_total = data.get("_remaining_bindings_total", len(bindings))
        suggestion = "Use umg.list_animation_bindings to view the complete remaining bindings list."

        # Binary search for max remaining_bindings count that fits
        lo, hi = 0, len(bindings)
        best = -1
        best_candidate = None

        while lo <= hi:
            mid = (lo + hi) // 2
            trial = dict(data)
            trial["remaining_bindings"] = bindings[:mid]
            trial["_remaining_bindings_truncated"] = True
            trial["_remaining_bindings_total"] = original_total
            trial["_remaining_bindings_returned"] = mid
            trial["_suggestion"] = suggestion
            trial["_remaining_bindings_instructions"] = suggestion
            trial_text = compact_json(trial)
            if len(trial_text) <= MAX_RESPONSE_CHARS:
                best = mid
                best_candidate = trial
                lo = mid + 1
            else:
                hi = mid - 1

        if best_candidate is not None:
            logger.info(
                "Truncated remaining_bindings response for %s: %d -> %d items",
                tool_name, len(bindings), best,
            )
            return compact_json(best_candidate)

        # If even 0 remaining_bindings didn't fit, check if diagnostics can be truncated
        if "diagnostics" in data and isinstance(data["diagnostics"], list):
            diag = data["diagnostics"]
            lo, hi = 0, len(diag)
            best_diag_candidate = None
            while lo <= hi:
                mid = (lo + hi) // 2
                trial = dict(data)
                trial["remaining_bindings"] = []
                trial["_remaining_bindings_truncated"] = True
                trial["_remaining_bindings_total"] = original_total
                trial["_remaining_bindings_returned"] = 0
                trial["_suggestion"] = suggestion
                trial["_remaining_bindings_instructions"] = suggestion
                trial["diagnostics"] = diag[:mid]
                trial_text = compact_json(trial)
                if len(trial_text) <= MAX_RESPONSE_CHARS:
                    best_diag_candidate = trial
                    lo = mid + 1
                else:
                    hi = mid - 1

            if best_diag_candidate is not None:
                return compact_json(best_diag_candidate)

        # Fallback to minimal essential outcome envelope
        essential_keys = {
            "asset_path", "animation_name", "dry_run", "changed", "save_attempted", "saved",
            "fingerprint", "matched_selector", "before", "after", "scene_data_removed", "save_error",
        }
        outcome = {k: v for k, v in data.items() if k in essential_keys}
        outcome["remaining_bindings"] = []
        outcome["_remaining_bindings_truncated"] = True
        outcome["_remaining_bindings_total"] = original_total
        outcome["_remaining_bindings_returned"] = 0
        outcome["_suggestion"] = suggestion
        outcome["_remaining_bindings_instructions"] = suggestion
        return compact_json(outcome)

    # 2. Reads that own an explicit selector are complete-or-refuse: name the selector instead of
    # shortening an array that would then look like a complete read. Native error envelopes are
    # never converted into a refusal.
    policy = _COMPLETE_OR_REFUSE_POLICIES.get(qualified_command or tool_name)
    if policy is not None and not data.get("_error"):
        return _refuse_oversized_read(
            data,
            qualified_command or tool_name,
            policy,
            request_params,
        )

    working_data = dict(data)

    # 3. Bound nested tracks in bindings if present and oversized
    if "bindings" in working_data and isinstance(working_data["bindings"], list):
        max_nested_tracks = 0
        for b in working_data["bindings"]:
            if isinstance(b, dict) and isinstance(b.get("tracks"), list):
                max_nested_tracks = max(max_nested_tracks, len(b["tracks"]))

        if max_nested_tracks > 5:
            # Binary search max tracks per binding
            lo, hi = 0, max_nested_tracks
            best_track_limit = -1
            best_bounded = None
            while lo <= hi:
                mid = (lo + hi) // 2
                trial = _bound_nested_tracks(working_data, mid)
                trial_text = compact_json(trial)
                if len(trial_text) <= MAX_RESPONSE_CHARS:
                    best_track_limit = mid
                    best_bounded = trial
                    lo = mid + 1
                else:
                    hi = mid - 1

            if best_bounded is not None:
                logger.info(
                    "Bounded nested tracks for %s: max %d tracks",
                    tool_name, best_track_limit,
                )
                return compact_json(best_bounded)

            # If even bounding tracks didn't completely fit, use 5 tracks as bounded baseline
            working_data = _bound_nested_tracks(working_data, 5)

    # 4. Truncate diagnostics separately if present and oversized (without corrupting binding pagination)
    if "diagnostics" in working_data and isinstance(working_data["diagnostics"], list) and len(working_data["diagnostics"]) > 0:
        diag = working_data["diagnostics"]
        orig_diag_count = len(diag)
        lo, hi = 0, orig_diag_count
        best_diag_count = -1
        best_diag_candidate = None
        while lo <= hi:
            mid = (lo + hi) // 2
            trial = dict(working_data)
            trial["diagnostics"] = diag[:mid]
            trial["_diagnostics_truncated"] = True
            trial["_diagnostics_total"] = orig_diag_count
            trial["_diagnostics_returned"] = mid
            trial_text = compact_json(trial)
            if len(trial_text) <= MAX_RESPONSE_CHARS:
                best_diag_count = mid
                best_diag_candidate = trial
                lo = mid + 1
            else:
                hi = mid - 1

        if best_diag_candidate is not None:
            logger.info(
                "Truncated diagnostics for %s: %d -> %d items",
                tool_name, orig_diag_count, best_diag_count,
            )
            return compact_json(best_diag_candidate)

        # If even 0 diagnostics doesn't fit, remove diagnostics from working_data
        working_data = dict(working_data)
        working_data["diagnostics"] = []
        working_data["_diagnostics_truncated"] = True
        working_data["_diagnostics_total"] = orig_diag_count
        working_data["_diagnostics_returned"] = 0

    # 5. Truncate the primary array (with pagination reconciliation if applicable)
    # Determine which array pagination belongs to
    paginated_key = None
    if "pagination" in working_data and isinstance(working_data["pagination"], dict):
        if "bindings" in working_data and isinstance(working_data["bindings"], list):
            paginated_key = "bindings"
        else:
            # Match by length or limit
            for k, v in working_data.items():
                if isinstance(v, list) and len(v) > 0:
                    paginated_key = k
                    break

    array_key = paginated_key or _find_largest_list(working_data)

    if array_key is None or not isinstance(working_data.get(array_key), list) or len(working_data[array_key]) == 0:
        logger.warning(
            "Response for %s is %d chars with no truncatable array",
            tool_name, len(compact_json(working_data)),
        )
        return compact_json({
            "_error": "RESPONSE_TOO_LARGE",
            "_size": len(compact_json(working_data)),
            "_suggestion": "Pass 'limit' parameter to paginate through results.",
        })

    target_list = working_data[array_key]
    original_count = len(target_list)

    lo, hi = 0, original_count
    best = 0
    while lo <= hi:
        mid = (lo + hi) // 2
        trial = dict(working_data)
        trial[array_key] = target_list[:mid]
        if array_key == paginated_key and "pagination" in trial and isinstance(trial["pagination"], dict):
            p = dict(trial["pagination"])
            p_offset = p.get("offset", 0)
            p_total = p.get("total", original_count)
            p["returned"] = mid
            if p_offset + mid < p_total:
                p["next_offset"] = p_offset + mid
                p["is_complete"] = False
            else:
                p["next_offset"] = None
                p["is_complete"] = True
            trial["pagination"] = p
        trial["_truncated"] = {
            "original_count": original_count,
            "returned_count": mid,
            "suggestion": "Pass 'limit' parameter to paginate through results.",
        }
        trial_text = compact_json(trial)
        if len(trial_text) <= MAX_RESPONSE_CHARS:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1

    if best == 0:
        logger.warning(
            "Response for %s cannot fit even a single item of %s under %d chars",
            tool_name, array_key, MAX_RESPONSE_CHARS,
        )
        return compact_json({
            "_error": "RESPONSE_TOO_LARGE",
            "_size": len(compact_json(working_data)),
            "_suggestion": f"Individual entries in '{array_key}' exceed size limit. Reduce nested fields or query with smaller scope.",
        })

    truncated = dict(working_data)
    truncated[array_key] = target_list[:best]
    if array_key == paginated_key and "pagination" in truncated and isinstance(truncated["pagination"], dict):
        p = dict(truncated["pagination"])
        p_offset = p.get("offset", 0)
        p_total = p.get("total", original_count)
        p["returned"] = best
        if p_offset + best < p_total:
            p["next_offset"] = p_offset + best
            p["is_complete"] = False
        else:
            p["next_offset"] = None
            p["is_complete"] = True
        truncated["pagination"] = p
    truncated["_truncated"] = {
        "original_count": original_count,
        "returned_count": best,
        "suggestion": "Pass 'limit' parameter to paginate through results.",
    }

    logger.info(
        "Truncated %s response for %s: %d -> %d items",
        array_key, tool_name, original_count, best,
    )
    return compact_json(truncated)
