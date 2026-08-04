################################################################################
# JAI Go-X Camera Driver (eBUS SDK for JAI)
# This is a subdirectory of the top-level AmigaDrivers project.
# spdlog and nlohmann/json are provided by the parent (FetchContent, targets
# spdlog::spdlog / nlohmann_json::nlohmann_json); doctest is vendored at the
# repo level in 3rd_party/.
# The eBUS SDK is a hard requirement (see the shared root cmake/FindeBUS.cmake,
# one Pleora SDK serves gox and fx10); the devcontainer installs it from
# fx10_driver/resource/.

# Build options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(GOX_BUILD_TESTS "Build gox unit tests (SDK-free)" ON)
else ()
    option(GOX_BUILD_TESTS "Build gox unit tests (SDK-free)" OFF)
endif ()

# Directory-scoped: applies only to targets defined in this directory
add_compile_options(-Wall -Wextra -Wpedantic)


################################################################################
# eBUS SDK (proprietary, installed out-of-tree; see root cmake/FindeBUS.cmake)
find_package(eBUS)
if (NOT eBUS_FOUND)
    message(FATAL_ERROR
        "eBUS SDK not found "
        "(expected under /opt/jai/ebus_sdk/<dist> or /opt/pleora/ebus_sdk/<dist>) "
        "or pass -DEBUS_SDK_ROOT=/path/to/sdk. The devcontainer installs it from "
        "fx10_driver/resource/ (see .devcontainer/Dockerfile).")
endif ()


################################################################################
# Compile-time GenICam env constants for ebus/env_bootstrap.cpp (directory-
# scoped, same mechanism as fx10). Values come from root cmake/FindeBUS.cmake.
if (NOT DEFINED EBUS_GENICAM_ROOT_DIR)
    set(EBUS_GENICAM_ROOT_DIR "")
endif ()
if (NOT DEFINED EBUS_GENICAM_ENV_NAME)
    set(EBUS_GENICAM_ENV_NAME "GENICAM_ROOT_V3_4")
endif ()
add_compile_definitions(
    GOX_GENICAM_ENV_NAME="${EBUS_GENICAM_ENV_NAME}"
    GOX_GENICAM_ROOT="${EBUS_GENICAM_ROOT_DIR}"
)


################################################################################
# jai_core - SDK-free capture core (config, format, recorder, logging facade)
# Headers live under include/ (include/core, include/ebus, ...); includes are
# rooted there, e.g. #include "core/config.hpp".
add_library(jai_core STATIC
        src/chunk_pool.cpp
        src/app_config.cpp
        src/format.cpp
        src/recorder.cpp
        src/signal_stop.cpp
        src/stats.cpp
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

# jai_ebus - eBUS SDK glue (discovery, camera control, streaming, PTP) +
# the shared capture orchestration (CaptureRunner)
add_library(jai_ebus STATIC
    src/capture_runner.cpp
    src/ebus/camera_controller.cpp
    src/ebus/camera_session.cpp
    src/ebus/discovery.cpp
    src/ebus/env_bootstrap.cpp
    src/ebus/ptp_manager.cpp
    src/ebus/sdk_error.cpp
    src/ebus/stream_receiver.cpp
)
# RPATH to the SDK/GenICam lib dirs propagates from eBUS::eBUS
# (INTERFACE_LINK_OPTIONS, see cmake/FindeBUS.cmake) to every consumer,
# including the top-level AmigaDrivers executable.
target_link_libraries(jai_ebus PUBLIC jai_core eBUS::eBUS)

# gox_lib - Amiga-facing wrapper layer (GoxDriverApp), consumed by the
# top-level AmigaDrivers executable
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

add_executable(jai_discover tools/discover_main.cpp)
target_link_libraries(jai_discover PRIVATE jai_ebus)

# jai_snapshot - one-shot frame grab for the web GUI preview loop
# (config/config-gox-snapshot.yaml + CLI overrides; see tools/snapshot_main.cpp)
add_executable(jai_snapshot tools/snapshot_main.cpp)
target_link_libraries(jai_snapshot PRIVATE jai_ebus)

set_target_properties(jai_discover jai_snapshot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)


################################################################################
# Tests (doctest, SDK-free; enable_testing() is called at the top level)
if (GOX_BUILD_TESTS)
    add_executable(jai_tests
        tests/doctest_main.cpp
        tests/test_config.cpp
        tests/test_format.cpp
        tests/test_frame_queue.cpp
        tests/test_recorder.cpp
        tests/test_stats.cpp
    )
    target_link_libraries(jai_tests PRIVATE jai_core)
    set_target_properties(jai_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests
    )
    add_test(NAME jai_tests COMMAND jai_tests)
endif ()


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   Gox_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  eBUS SDK:         ${EBUS_SDK_ROOT_DIR}")
message(STATUS "  Build Tests:      ${GOX_BUILD_TESTS}")
message(STATUS "----------------------------------------")
message(STATUS "")
