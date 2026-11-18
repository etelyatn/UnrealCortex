"""Cache-owned reads called without ``limit`` return page 1 with a cursor (issue #225).

The generic truncator used to binary-search the array and suggest a ``limit`` the read's
owner does consume — but the truncated rows were lost unless the model re-ran the whole
read with a guessed page size. The executed read is snapshotted instead, so page 1 plus
``cursor`` continuations retrieve every row without a second native dispatch.
"""

import json

from cortex_mcp.response import MAX_RESPONSE_CHARS, compact_json
from cortex_mcp.tools import routers as routers_mod
from cortex_mcp.tools.routers import make_router

from test_pagination_contract import _actors, _router


def test_oversized_no_limit_read_returns_page_one_with_cursor():
    router, connection = _router(
        "level", {"matches": _actors(30, blob=4000), "count": 30, "pattern": "Actor"}
    )

    payload = json.loads(router("find_actors", {"pattern": "Actor"}))

    # The whole read was snapshotted; page 1 is the largest page that fits.
    assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS
    returned = len(payload["matches"])
    assert 0 < returned < 30
    assert payload["_pagination"]["total"] == 30
    assert payload["_pagination"]["returned"] == returned
    cursor = payload["_pagination"]["next_cursor"]
    assert cursor is not None
    # Only one native dispatch: the caller did not pay for a repeat read.
    assert connection.send_command.call_count == 1


def test_no_limit_cursor_continuation_covers_every_row_exactly_once():
    router, _ = _router(
        "level", {"matches": _actors(30, blob=4000), "count": 30, "pattern": "Actor"}
    )

    seen = []
    payload = json.loads(router("find_actors", {"pattern": "Actor"}))
    seen.extend(item["name"] for item in payload["matches"])
    while payload["_pagination"]["next_cursor"] is not None:
        payload = json.loads(router("find_actors", {"cursor": payload["_pagination"]["next_cursor"]}))
        seen.extend(item["name"] for item in payload["matches"])

    assert seen == [f"Actor_{i}" for i in range(30)]


def test_fitting_no_limit_read_is_returned_whole_without_pagination():
    router, connection = _router(
        "level", {"matches": _actors(5), "count": 5, "pattern": "Actor"}
    )

    payload = json.loads(router("find_actors", {"pattern": "Actor"}))

    assert len(payload["matches"]) == 5
    assert "_pagination" not in payload
    assert connection.send_command.call_count == 1


def test_missing_mapped_array_still_falls_back_to_format_response():
    # The owner did not return its primary array: the read stays complete rather than
    # being snapshotted into a page the caller cannot trust.
    router, _ = _router("level", {"error": "world unavailable"})

    payload = json.loads(router("find_actors", {"pattern": "Actor"}))

    assert payload == {"error": "world unavailable"}


def test_batch_query_overflow_path_still_pages_after_change():
    # batch_query shares the snapshot machinery; its no-limit overflow behaviour is
    # already covered by its own tests and must not regress here.
    router, connection = _router(
        "core", {"results": [{"ok": True, "blob": "x" * 5000} for _ in range(30)]}
    )

    payload = json.loads(router("batch_query", {"commands": [{"command": "ping"}]}))

    assert len(compact_json(payload)) <= MAX_RESPONSE_CHARS
    assert payload["_pagination"]["next_cursor"] is not None
    assert connection.send_command.call_count == 1


def test_boundary_row_still_returns_rows_instead_of_stranding_the_read():
    # A first row that fits only without the continuation envelope: the generic truncation
    # (row + `_truncated`) must be used instead of a refusal that strands every row.
    # Measured band: with this payload shape the page envelope refuses at blob ~39,620-39,740
    # while the generic envelope still returns the first row.
    blob = MAX_RESPONSE_CHARS - 300
    router, connection = _router(
        "level", {"matches": _actors(12, blob=blob), "count": 12, "pattern": "Actor"}
    )

    payload = json.loads(router("find_actors", {"pattern": "Actor"}))

    assert "matches" in payload, payload
    assert len(payload["matches"]) >= 1
    assert (
        payload.get("_pagination", {}).get("next_cursor") is not None
        or payload.get("_truncated", {}).get("returned_count", 0) >= 1
    )
    assert connection.send_command.call_count == 1
