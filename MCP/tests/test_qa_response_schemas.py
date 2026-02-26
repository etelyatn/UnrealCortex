"""Response schema contracts for QA MCP tools."""


def assert_vector_object(value, field_name):
    assert isinstance(value, dict), f"{field_name} must be dict, got {type(value)}"
    for key in ("x", "y", "z"):
        assert key in value, f"{field_name} missing key '{key}'"
        assert isinstance(value[key], (int, float)), f"{field_name}.{key} must be numeric"


def assert_rotator_object(value, field_name):
    assert isinstance(value, dict), f"{field_name} must be dict, got {type(value)}"
    for key in ("pitch", "yaw", "roll"):
        assert key in value, f"{field_name} missing key '{key}'"
        assert isinstance(value[key], (int, float)), f"{field_name}.{key} must be numeric"


def test_teleport_player_schema_contract():
    response = {
        "success": True,
        "location": {"x": 100.0, "y": 50.0, "z": 20.0},
        "rotation": {"pitch": 0.0, "yaw": 90.0, "roll": 0.0},
        "control_rotation": {"pitch": 0.0, "yaw": 90.0, "roll": 0.0},
    }

    assert isinstance(response["success"], bool)
    assert_vector_object(response["location"], "location")
    assert_rotator_object(response["rotation"], "rotation")
    assert_rotator_object(response["control_rotation"], "control_rotation")


def test_probe_forward_schema_contract():
    response = {
        "hit": True,
        "distance": 155.0,
        "location": {"x": 0.0, "y": 0.0, "z": 100.0},
        "surface_normal": {"x": 0.0, "y": 1.0, "z": 0.0},
        "hit_type": "actor",
        "look_yaw": 15.0,
        "look_pitch": -2.0,
        "actor": {
            "name": "Door_01",
            "path": "/Game/Test/Door_01",
            "class": "BP_Door_C",
            "tags": ["Interactable"],
            "in_interaction_range": True,
        },
    }

    assert isinstance(response["hit"], bool)
    assert isinstance(response["distance"], (int, float))
    assert_vector_object(response["location"], "location")
    assert_vector_object(response["surface_normal"], "surface_normal")
    assert response["hit_type"] in {"actor", "geometry", "none"}
    assert isinstance(response["look_yaw"], (int, float))
    assert isinstance(response["look_pitch"], (int, float))


def test_check_stuck_schema_contract():
    response = {
        "is_stuck": False,
        "distance_moved": 30.0,
        "threshold": 10.0,
        "duration": 0.5,
        "start_location": {"x": 0.0, "y": 0.0, "z": 0.0},
        "end_location": {"x": 20.0, "y": 0.0, "z": 0.0},
        "velocity": {"x": 200.0, "y": 0.0, "z": 0.0},
        "speed": 200.0,
        "is_falling": False,
        "obstruction": None,
    }

    assert isinstance(response["is_stuck"], bool)
    assert isinstance(response["distance_moved"], (int, float))
    assert isinstance(response["threshold"], (int, float))
    assert isinstance(response["duration"], (int, float))
    assert_vector_object(response["start_location"], "start_location")
    assert_vector_object(response["end_location"], "end_location")
    assert_vector_object(response["velocity"], "velocity")
    assert isinstance(response["speed"], (int, float))
    assert isinstance(response["is_falling"], bool)


def test_get_visible_actors_schema_contract():
    response = {
        "camera_location": {"x": 0.0, "y": 0.0, "z": 100.0},
        "camera_rotation": {"pitch": 0.0, "yaw": 90.0, "roll": 0.0},
        "camera_fov": 90.0,
        "count": 1,
        "actors": [
            {
                "name": "Enemy_01",
                "class": "BP_Enemy_C",
                "location": {"x": 250.0, "y": 350.0, "z": 20.0},
                "distance": 430.0,
                "direction": "ahead",
                "relative_angle": 10.0,
                "tags": ["Enemy"],
                "in_interaction_range": False,
                "in_line_of_sight": True,
                "yaw_offset": 8.0,
                "pitch_offset": -1.0,
                "look_at_yaw": 98.0,
                "look_at_pitch": -1.0,
                "path": "/Game/Test/Enemy_01",
            }
        ],
    }

    assert_vector_object(response["camera_location"], "camera_location")
    assert_rotator_object(response["camera_rotation"], "camera_rotation")
    assert isinstance(response["camera_fov"], (int, float))
    assert isinstance(response["count"], int)
    assert isinstance(response["actors"], list)
    assert len(response["actors"]) == response["count"]
    assert_vector_object(response["actors"][0]["location"], "actors[0].location")
