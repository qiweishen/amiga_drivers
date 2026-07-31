################################################################################
# Specim FX10e Hyperspectral Camera Driver (Pleora eBUS SDK 6.x)
# This is a subdirectory of the top-level AmigaDrivers project.
# spdlog/yaml-cpp come transitively from amiga_common; nlohmann/json is the
# parent FetchContent target. The eBUS SDK is a hard requirement (see the
# shared root cmake/FindeBUS.cmake); the devcontainer installs it from
# fx10_driver/resource/.

# Build options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(FX10_BUILD_TESTS "Build fx10 unit tests (SDK-free)" ON)
else ()
    option(FX10_BUILD_TESTS "Build fx10 unit tests (SDK-free)" OFF)
endif ()

# Warnings apply to fx10 targets only; eBUS headers are SYSTEM via eBUS::eBUS
add_library(fx10_warnings INTERFACE)
target_compile_options(fx10_warnings INTERFACE -Wall -Wextra -Wpedantic)


################################################################################
# eBUS SDK (proprietary, installed out-of-tree; see root cmake/FindeBUS.cmake)
find_package(eBUS REQUIRED)

# Compile-time GenICam env constants for fx10/ebus_env.hpp (directory-scoped:
# every fx10 target may include the bootstrap header)
add_compile_definitions(
    FX10_GENICAM_ENV_NAME="${EBUS_GENICAM_ENV_NAME}"
    FX10_GENICAM_ROOT="${EBUS_GENICAM_ROOT_DIR}"
)


################################################################################
# fx10_core - SDK-free library (config, recorder, accounting); unit-testable,
# must never include an eBUS header
add_library(fx10_core STATIC
        src/accounting.cpp
        src/pixel_format.cpp
        src/buffer_sizing.cpp
        src/app_config.cpp
        src/envi_header.cpp
        src/envi_recorder.cpp
        src/timestamp_sidecar.cpp
        src/wavelengths.cpp
)
# NOTE: gox_driver/include exposes the SAME relative paths (core/..., ebus/...).
# Safe today because no TU sees both roots (main.cpp only includes the two
# self-contained *_driver_app.h); never include "core/..." from a header that
# main.cpp pulls in.
target_include_directories(fx10_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# amiga_common carries the unified logging layer and provides spdlog/yaml-cpp/
# Threads transitively
target_link_libraries(fx10_core
    PUBLIC amiga_common
    PRIVATE fx10_warnings
)


################################################################################
# fx10_ebus - eBUS SDK glue (camera control + streaming). RPATH to the SDK and
# GenICam lib dirs propagates from eBUS::eBUS (INTERFACE_LINK_OPTIONS) to every
# consumer, including the top-level AmigaDrivers executable.
add_library(fx10_ebus STATIC
    src/ebus/camera_control.cpp
    src/ebus/stream_receiver.cpp
)
target_link_libraries(fx10_ebus
    PUBLIC fx10_core eBUS::eBUS
    PRIVATE fx10_warnings
)


################################################################################
# fx10_lib - Amiga-facing wrapper layer (Fx10DriverApp), consumed by the
# top-level AmigaDrivers executable
add_library(fx10_lib STATIC
    src/fx10_driver_app.cpp
)
target_link_libraries(fx10_lib PUBLIC
    fx10_ebus
    amiga_common
)


################################################################################
# fx10_snapshot - one-shot GUI preview grab + --list discovery (stdout contract
# in tools/snapshot_main.cpp: last line "SNAPSHOT: OK|FAIL ..." / JSON)
add_executable(fx10_snapshot tools/snapshot_main.cpp)
target_link_libraries(fx10_snapshot PRIVATE fx10_ebus nlohmann_json::nlohmann_json fx10_warnings)
set_target_properties(fx10_snapshot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)


################################################################################
# Tests (GoogleTest, SDK-free; enable_testing() is called at the top level)
if (FX10_BUILD_TESTS)
    add_subdirectory(tests)
endif ()


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   Fx10_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  eBUS SDK:         ${EBUS_SDK_ROOT_DIR}")
message(STATUS "  Build Tests:      ${FX10_BUILD_TESTS}")
message(STATUS "----------------------------------------")
message(STATUS "")
