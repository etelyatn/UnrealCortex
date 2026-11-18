"""Routed `reflect.class_detail` keeps the former wrapper's properties view (issue #225).

The six thin reflect wrappers were removed; `detail="properties"` was the one view the
native command cannot express (it implements `summary`/`full` only). The router maps it to a
native full read and projects the function signatures out before response budgeting.
"""

import json
from unittest.mock import MagicMock

from cortex_mcp.tools.routers import make_router

_FULL_DETAIL = {
    "name": "AMyActor",
    "type": "cpp",
    "parent": "AActor",
    "module": "MyGame",
    "source_path": "MyActor.h",
    "blueprint_children_count": 2,
    "interfaces": [{"name": "IThing"}],
    "properties": [{"name": "Health", "type": "float"}],
    "functions": [{"name": f"Func_{i}", "return_type": "void"} for i in range(50)],
    "components": [{"name": "Mesh", "class": "UStaticMeshComponent"}],
    "cdo_unavailable": True,
    "_message": "component defaults unavailable",
}


def _router(data=None):
    connection = MagicMock()
    connection.send_command.return_value = {"success": True, "data": data or _FULL_DETAIL}
    return make_router("reflect", connection, "reflect docs"), connection


def _sent(connection):
    call = connection.send_command.call_args_list[-1]
    command, params = call.args[0], call.args[1]
    return command, params


def test_properties_view_forwards_native_full_and_projects_functions_out():
    router, connection = _router()

    payload = json.loads(
        router("class_detail", {"class_name": "AMyActor", "detail": "properties"})
    )

    command, params = _sent(connection)
    assert command == "reflect.class_detail"
    assert params["detail"] == "full"  # native implements summary/full only
    assert "functions" not in payload
    assert payload["properties"] == _FULL_DETAIL["properties"]
    assert payload["interfaces"] == _FULL_DETAIL["interfaces"]
    assert payload["components"] == _FULL_DETAIL["components"]
    # Coverage/error metadata is not a presentation level and must survive the projection.
    assert payload["cdo_unavailable"] is True
    assert payload["_message"] == "component defaults unavailable"


def test_properties_view_forwards_include_inherited():
    router, connection = _router()

    router(
        "class_detail",
        {"class_name": "AMyActor", "detail": "properties", "include_inherited": True},
    )

    _, params = _sent(connection)
    assert params["detail"] == "full"
    assert params["include_inherited"] is True


def test_default_and_summary_reads_are_forwarded_unchanged():
    router, connection = _router()

    router("class_detail", {"class_name": "AMyActor"})
    _, params = _sent(connection)
    assert "detail" not in params

    payload = json.loads(
        router("class_detail", {"class_name": "AMyActor", "detail": "summary"})
    )
    _, params = _sent(connection)
    assert params["detail"] == "summary"
    assert "functions" in payload  # summary data here is the stub's full payload; forwarded untouched
