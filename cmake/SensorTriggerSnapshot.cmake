# Content identity of the local host client used by this project. No upstream
# revision is inferred: updating these hashes requires reviewing the changed
# host API/protocol together. Firmware is not built by AmigaDrivers.
set(sensor_trigger_SOURCE_DIR "${CMAKE_SOURCE_DIR}/3rd_party/External/sensor_trigger")
set(_sensor_trigger_snapshot
    "session_client.cpp=2d53ed19a7590198b56419193d9499bf2bc660b8fed2dc77c63fb4aec5d78443"
    "session_protocol.h=1caf58af75354fcb9c74d1647c16ffde91be876af731915c15bb06f7f29775ca"
    "session_rates.h=5e356e92388230297ebd4fe2bbc01233af4f3159f26107dc4ba645abc814581c"
    "trigger_groups.h=7ceb5e2ff9c95d783b5d575b14c7fca77eca61e37fe4f410077c8a6d4422e6f7")
foreach(_entry IN LISTS _sensor_trigger_snapshot)
    string(REPLACE "=" ";" _parts "${_entry}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _expected)
    set(_path "${sensor_trigger_SOURCE_DIR}/${_name}")
    if (NOT EXISTS "${_path}")
        message(FATAL_ERROR "Missing pinned SensorSync source: ${_path}. Supply the reviewed local snapshot; configure will not download it.")
    endif ()
    file(SHA256 "${_path}" _actual)
    if (NOT _actual STREQUAL _expected)
        message(FATAL_ERROR "SensorSync snapshot mismatch: ${_name}. Review the host/protocol changes before updating cmake/SensorTriggerSnapshot.cmake.")
    endif ()
endforeach ()
