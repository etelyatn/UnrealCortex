"""Response size guard for MCP tool results."""

import json
import logging

logger = logging.getLogger(__name__)

_MAX_RESPONSE_CHARS = 40_000


def format_response(data: dict, tool_name: str) -> str:
    """Serialize data to JSON, truncating array results if over size limit.

    If the response exceeds _MAX_RESPONSE_CHARS and contains a truncatable
    array ('rows', 'results', 'tags', 'datatables', 'data_assets',
    'string_tables', 'assets', 'entries'), binary-searches for the max
    array length that fits and appends _truncated metadata.

    Args:
        data: The response data dict.
        tool_name: Name of the tool for error messages.

    Returns:
        JSON string, guaranteed under _MAX_RESPONSE_CHARS.
    """
    text = json.dumps(data, indent=2)
    if len(text) <= _MAX_RESPONSE_CHARS:
        return text

    # Find a truncatable array key
    truncatable_keys = [
        "rows", "results", "tags", "datatables", "data_assets",
        "string_tables", "assets", "entries", "resolved",
    ]
    array_key = None
    for key in truncatable_keys:
        if key in data and isinstance(data[key], list) and len(data[key]) > 0:
            array_key = key
            break

    if array_key is None:
        logger.warning(
            "Response for %s is %d chars with no truncatable array",
            tool_name, len(text),
        )
        return json.dumps({
            "_error": "response_too_large",
            "_size": len(text),
            "_suggestion": f"Use 'fields' parameter to select only needed fields, "
                           f"or reduce 'limit' to get fewer rows.",
        }, indent=2)

    original_count = len(data[array_key])

    # Binary search for max count that fits
    lo, hi = 0, original_count
    best = 0
    while lo <= hi:
        mid = (lo + hi) // 2
        trial = dict(data)
        trial[array_key] = data[array_key][:mid]
        trial["_truncated"] = {
            "original_count": original_count,
            "returned_count": mid,
            "suggestion": "Use 'fields' parameter to select only needed fields, "
                          "or reduce 'limit' to get fewer rows.",
        }
        trial_text = json.dumps(trial, indent=2)
        if len(trial_text) <= _MAX_RESPONSE_CHARS:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1

    truncated = dict(data)
    truncated[array_key] = data[array_key][:best]
    truncated["_truncated"] = {
        "original_count": original_count,
        "returned_count": best,
        "suggestion": "Use 'fields' parameter to select only needed fields, "
                      "or reduce 'limit' to get fewer rows.",
    }

    logger.info(
        "Truncated %s response for %s: %d -> %d items",
        array_key, tool_name, original_count, best,
    )
    return json.dumps(truncated, indent=2)
