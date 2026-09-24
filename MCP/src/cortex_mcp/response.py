"""Response size guard for MCP tool results."""

import json
import logging

logger = logging.getLogger(__name__)

MAX_RESPONSE_CHARS = 40_000
_MIN_LIST_SIZE = 10


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


def format_response(data: dict, tool_name: str) -> str:
    """Serialize data to JSON, truncating array results if over size limit.

    If the response exceeds MAX_RESPONSE_CHARS:
    1. For mutation results with remaining_bindings, essential outcome fields
       are always preserved, and remaining_bindings is truncated with dedicated metadata.
    2. For binding queries, nested track details are bounded first so canonical selectors
       remain retrievable under the 40k limit.
    3. Diagnostics are truncated separately with independent metadata without corrupting
       binding pagination.
    4. Paginated arrays are truncated with reconciled pagination metadata guaranteeing
       forward progress.

    Args:
        data: The response data dict.
        tool_name: Name of the tool for error messages.

    Returns:
        JSON string, guaranteed under MAX_RESPONSE_CHARS.
    """
    text = json.dumps(data, indent=2)
    if len(text) <= MAX_RESPONSE_CHARS:
        return text

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
            trial_text = json.dumps(trial, indent=2)
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
            return json.dumps(best_candidate, indent=2)

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
                trial_text = json.dumps(trial, indent=2)
                if len(trial_text) <= MAX_RESPONSE_CHARS:
                    best_diag_candidate = trial
                    lo = mid + 1
                else:
                    hi = mid - 1

            if best_diag_candidate is not None:
                return json.dumps(best_diag_candidate, indent=2)

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
        return json.dumps(outcome, indent=2)

    working_data = dict(data)

    # 2. Bound nested tracks in bindings if present and oversized
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
                trial_text = json.dumps(trial, indent=2)
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
                return json.dumps(best_bounded, indent=2)

            # If even bounding tracks didn't completely fit, use 5 tracks as bounded baseline
            working_data = _bound_nested_tracks(working_data, 5)

    # 3. Truncate diagnostics separately if present and oversized (without corrupting binding pagination)
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
            trial_text = json.dumps(trial, indent=2)
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
            return json.dumps(best_diag_candidate, indent=2)

        # If even 0 diagnostics doesn't fit, remove diagnostics from working_data
        working_data = dict(working_data)
        working_data["diagnostics"] = []
        working_data["_diagnostics_truncated"] = True
        working_data["_diagnostics_total"] = orig_diag_count
        working_data["_diagnostics_returned"] = 0

    # 4. Truncate the primary array (with pagination reconciliation if applicable)
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
            tool_name, len(json.dumps(working_data)),
        )
        return json.dumps({
            "_error": "RESPONSE_TOO_LARGE",
            "_size": len(json.dumps(working_data)),
            "_suggestion": "Pass 'limit' parameter to paginate through results.",
        }, indent=2)

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
        trial_text = json.dumps(trial, indent=2)
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
        return json.dumps({
            "_error": "RESPONSE_TOO_LARGE",
            "_size": len(json.dumps(working_data)),
            "_suggestion": f"Individual entries in '{array_key}' exceed size limit. Reduce nested fields or query with smaller scope.",
        }, indent=2)

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
    return json.dumps(truncated, indent=2)
