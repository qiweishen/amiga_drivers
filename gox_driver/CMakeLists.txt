################################################################################
# JAI Go-X Camera Driver (eBUS SDK for JAI)
# This is a subdirectory of the top-level AmigaDrivers project.
# spdlog is provided by the parent (FetchContent, target spdlog::spdlog);
# nlohmann/json and doctest remain vendored in third_party/.
# The eBUS SDK is a hard requirement (see cmake/FindeBUS.cmake); the
# devcontainer installs it from gox_driver/resource/.

# Build options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(GOX_BUILD_TESTS "Build gox unit tests (SDK-free)" ON)
else ()
    option(GOX_BUILD_TESTS "Build gox unit tests (SDK-free)" OFF)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Directory-scoped: applies only to targets defined in this directory
add_compile_options(-Wall -Wextra -Wpedantic)


################################################################################
# eBUS SDK (proprietary, installed out-of-tree; see cmake/FindeBUS.cmake)
find_package(eBUS)
if (NOT eBUS_FOUND)
    message(FATAL_ERROR
        "eBUS SDK not found. Install the eBUS SDK for JAI "
        "(expected under /opt/jai/ebus_sdk/<dist> or /opt/pleora/ebus_sdk/<dist>) "
        "or pass -DEBUS_SDK_ROOT=/path/to/sdk. The devcontainer installs it from "
        "gox_driver/resource/ (see .devcontainer/Dockerfile).")
endif ()


################################################################################
# Generated version.hpp (driver version, git SHA, compile-time SDK/GenICam
# paths consumed by ebus/env_bootstrap.cpp)
set(GOX_DRIVER_VERSION "0.1.0")
if (NOT DEFINED EBUS_SDK_ROOT_DIR)
    set(EBUS_SDK_ROOT_DIR "")
endif ()
if (NOT DEFINED EBUS_GENICAM_ROOT_DIR)
    set(EBUS_GENICAM_ROOT_DIR "")
endif ()
if (NOT DEFINED EBUS_GENICAM_ENV_NAME)
    set(EBUS_GENICAM_ENV_NAME "GENICAM_ROOT_V3_4")
endif ()
find_package(Git QUIET)
set(JAI_DRIVER_GIT_SHA "unknown")
if (GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE JAI_DRIVER_GIT_SHA_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE GIT_SHA_RESULT)
    if (GIT_SHA_RESULT EQUAL 0)
        set(JAI_DRIVER_GIT_SHA "${JAI_DRIVER_GIT_SHA_RAW}")
    endif ()
endif ()
configure_file(src/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)


################################################################################
# jai_core - SDK-free capture core (config, format, recorder, logging facade)
# Headers live under include/ (include/core, include/ebus, ...); includes are
# rooted there, e.g. #include "core/config.hpp".
add_library(jai_core STATIC
    src/core/chunk_pool.cpp
    src/core/config.cpp
    src/core/format.cpp
    src/core/logger.cpp
    src/core/recorder.cpp
    src/core/session_log.cpp
    src/core/signal_stop.cpp
    src/core/stats.cpp
    src/core/util.cpp
)
target_include_directories(jai_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
# SYSTEM: keep -Wall/-Wextra/-Wpedantic from firing inside vendored headers
# (nlohmann/json, doctest).
target_include_directories(jai_core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(jai_core PUBLIC
    spdlog::spdlog
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
    src/ebus/preflight.cpp
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
# (config/config-snapshot.json + CLI overrides; see tools/snapshot_main.cpp)
add_executable(jai_snapshot tools/snapshot_main.cpp)
target_link_libraries(jai_snapshot PRIVATE jai_ebus)

set_target_properties(jai_discover jai_snapshot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)


################################################################################
# SDK-free tools
add_executable(jai_fake_capture tools/fake_capture_main.cpp)
target_link_libraries(jai_fake_capture PRIVATE jai_core)
set_target_properties(jai_fake_capture PROPERTIES
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
