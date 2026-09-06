################################################################################
# JAI Go-X Camera Driver (eBUS SDK for JAI)
# This is a subdirectory of the top-level AmigaDrivers project.
# spdlog and nlohmann/json are provided by the parent (FetchContent, targets
# spdlog::spdlog / nlohmann_json::nlohmann_json); doctest is vendored at the
# repo level in 3rd_party/.
# The eBUS SDK is a hard requirement (see the shared root cmake/FindeBUS.cmake,
# one Pleora SDK serves gox and fx10); the devcontainer installs it from
# resource/ (repo root).

# Directory-scoped: applies only to targets defined in this directory
add_compile_options(-Wall -Wextra -Wpedantic)


################################################################################
# eBUS SDK (proprietary, installed out-of-tree; see root cmake/FindeBUS.cmake).
# common/ already failed the configure with a helpful message when it is
# missing; the call here only (re)creates the directory-scoped eBUS::eBUS target.
find_package(eBUS REQUIRED)


################################################################################
# jai_core
add_library(jai_core STATIC
        src/app_config.cpp
        src/apply_plan.cpp
        src/block_id_tracker.cpp
        src/buffering.cpp
        src/chunk_pool.cpp
        src/device_json.cpp
        src/format.cpp
        src/ptp_status.cpp
        src/recorder.cpp
        src/signal_stop.cpp
        src/stats.cpp
        src/user_set.cpp
        src/util.cpp
)
target_include_directories(jai_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# SYSTEM: keep -Wall/-Wextra/-Wpedantic from firing inside vendored headers
# (doctest).
target_include_directories(jai_core SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/3rd_party
)
# amiga_common carries the unified logging layer and provides spdlog/Threads/
# nlohmann (via drivers_json) transitively
target_link_libraries(jai_core PUBLIC
    amiga_common
    Threads::Threads
)


################################################################################
# SDK-dependent targets

# jai_ebus - eBUS SDK glue (camera control, streaming, PTP) + the shared
# capture orchestration (CaptureRunner). Discovery, the GenICam env bootstrap
# and the PvResult error plumbing come from common/ (amiga_ebus).
add_library(jai_ebus STATIC
    src/capture_runner.cpp
    src/ebus/camera_controller.cpp
    src/ebus/camera_session.cpp
    src/ebus/ptp_manager.cpp
    src/ebus/stream_receiver.cpp
)
# RPATH to the SDK/GenICam lib dirs propagates from eBUS::eBUS
# (INTERFACE_LINK_OPTIONS, see cmake/FindeBUS.cmake) to every consumer,
# including the top-level AmigaDrivers executable.
target_link_libraries(jai_ebus PUBLIC jai_core amiga_ebus eBUS::eBUS)

# gox_lib - Amiga-facing wrapper layer (GoxDriverApp)
add_library(gox_lib STATIC
    src/gox_driver_app.cpp
)
target_include_directories(gox_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(gox_lib PUBLIC
    jai_ebus
    # Shared common library provides spdlog, yaml-cpp, Threads
    amiga_common
)

# jai_snapshot - one-shot frame grab for the web GUI preview loop (device
# enumeration is the common ebus_discover tool)
add_executable(jai_snapshot tools/snapshot_main.cpp)
target_link_libraries(jai_snapshot PRIVATE jai_ebus)

set_target_properties(jai_snapshot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)


################################################################################
# Tests (doctest, SDK-free; enable_testing() is called at the top level)
if (AMIGA_BUILD_TESTS)
    set(GOX_TEST_SOURCES
        tests/test_apply_plan.cpp
        tests/test_block_id_tracker.cpp
        tests/test_buffering.cpp
        tests/test_config.cpp
        tests/test_device_json.cpp
        tests/test_format.cpp
        tests/test_ptp_status.cpp
        tests/test_recorder.cpp
        tests/test_signal_stop.cpp
        tests/test_stats.cpp
        tests/test_templates.cpp
        tests/test_user_set.cpp
        tests/test_util.cpp
    )
    add_executable(gox_tests tests/doctest_main.cpp ${GOX_TEST_SOURCES})
    target_link_libraries(gox_tests PRIVATE jai_core)
    # The shipped templates are asserted against the (strict) loader
    target_compile_definitions(gox_tests PRIVATE GOX_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config")
    set_target_properties(gox_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)
    # One ctest entry per source file (doctest --source-file filter)
    foreach (test_source IN LISTS GOX_TEST_SOURCES)
        get_filename_component(test_name ${test_source} NAME_WE)
        add_test(NAME gox.${test_name} COMMAND gox_tests --source-file=*${test_name}.cpp)
    endforeach ()
endif ()


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   Gox_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  eBUS SDK:         ${EBUS_SDK_ROOT_DIR}")
message(STATUS "  Build Tests:      ${AMIGA_BUILD_TESTS}")
message(STATUS "----------------------------------------")
message(STATUS "")
