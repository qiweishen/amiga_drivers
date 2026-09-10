################################################################################
# Specim FX10e Hyperspectral Camera Driver (Pleora eBUS SDK 6.x)
# This is a subdirectory of the top-level AmigaDrivers project.


# Warnings apply to fx10 targets only; eBUS headers are SYSTEM via eBUS::eBUS
add_library(fx10_warnings INTERFACE)
target_compile_options(fx10_warnings INTERFACE -Wall -Wextra -Wpedantic)


################################################################################
# eBUS SDK (proprietary, installed out-of-tree; see root cmake/FindeBUS.cmake).
# The GenICam env bootstrap, discovery and PvResult plumbing come from common/ (amiga_ebus);
# this call only (re)creates the directory-scoped eBUS::eBUS target.
find_package(eBUS REQUIRED)


################################################################################
# fx10_core - SDK-free library (config, recorder, accounting)
add_library(fx10_core STATIC
        src/accounting.cpp
        src/pixel_format.cpp
        src/recording_pipeline.cpp
        src/session_metadata.cpp
        src/buffer_sizing.cpp
        src/app_config.cpp
        src/apply_plan.cpp
        src/envi_header.cpp
        src/envi_recorder.cpp
        src/sensor_trigger_log.cpp
        src/stats_line.cpp
        src/wavelengths.cpp
)

# SensorSync-Logger host client
set(SENSOR_TRIGGER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/External/sensor_trigger")
target_include_directories(fx10_core PRIVATE "${SENSOR_TRIGGER_DIR}")
# NOTE: gox_driver/include exposes the SAME relative paths (core/..., ebus/...)
target_include_directories(fx10_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# amiga_common carries the unified logging layer and provides spdlog/yaml-cpp/
# Threads transitively
target_link_libraries(fx10_core
    PUBLIC amiga_common
    PRIVATE fx10_warnings
)


################################################################################
# fx10_ebus - eBUS SDK glue (camera control + streaming)
add_library(fx10_ebus STATIC
    src/ebus/camera_control.cpp
    src/ebus/stream_receiver.cpp
)
target_link_libraries(fx10_ebus
    PUBLIC fx10_core amiga_ebus eBUS::eBUS
    PRIVATE fx10_warnings
)


################################################################################
# fx10_lib - Amiga-facing wrapper layer (Fx10DriverApp)
add_library(fx10_lib STATIC
    src/fx10_driver_app.cpp
)
target_link_libraries(fx10_lib PUBLIC
    fx10_ebus
    amiga_common
)


################################################################################
# fx10_snapshot - one-shot GUI preview grab
add_executable(fx10_snapshot tools/snapshot_main.cpp)
target_link_libraries(fx10_snapshot PRIVATE fx10_ebus fx10_warnings)
set_target_properties(fx10_snapshot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Paired white/dark reference capture, preserving the saved acquisition settings.
add_executable(fx10_reference tools/reference_main.cpp)
target_link_libraries(fx10_reference PRIVATE fx10_ebus fx10_warnings)
set_target_properties(fx10_reference PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)


################################################################################
# Tests (GoogleTest, SDK-free; enable_testing() is called at the top level)
if (AMIGA_BUILD_TESTS)
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
message(STATUS "  Build Tests:      ${AMIGA_BUILD_TESTS}")
message(STATUS "----------------------------------------")
message(STATUS "")
