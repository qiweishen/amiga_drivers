################################################################################
# common - Shared infrastructure library for all Amiga drivers
# Provides: logging, config loading, signal handling, SPSC ring buffer


add_library(amiga_common STATIC
        src/drivers_json.cpp
        src/ebus/ipv4.cpp
        src/logger.cpp
        src/run_guards.cpp
        src/sensor_sync_hub.cpp
        src/sensor_sync_log.cpp
        src/signal_handler.cpp
        src/utility.cpp
)

set_target_properties(amiga_common PROPERTIES
        CXX_EXTENSIONS OFF
)

target_include_directories(amiga_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# SensorSync-Logger host client (session_client.cpp is header-included by
# sensor_sync_log.cpp; trigger_groups.h / session_rates.h are shared with the
# firmware). PUBLIC: the camera drivers validate pulse rates with trigger_groups.h
set(SENSOR_TRIGGER_DIR "${CMAKE_SOURCE_DIR}/3rd_party/External/sensor_trigger")
target_include_directories(amiga_common SYSTEM PUBLIC "${SENSOR_TRIGGER_DIR}")

target_link_libraries(amiga_common PUBLIC
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        nlohmann_json::nlohmann_json
        Threads::Threads
)


################################################################################
# Tests (doctest, vendored in 3rd_party/)
if (AMIGA_BUILD_TESTS)
    add_subdirectory(tests)
endif ()


################################################################################
# amiga_ebus - the eBUS-SDK-dependent layer shared by gox_driver and fx10_driver
find_package(eBUS)
if (NOT eBUS_FOUND)
    message(FATAL_ERROR
            "eBUS SDK not found "
            "(expected under /opt/jai/ebus_sdk/<dist> or /opt/pleora/ebus_sdk/<dist>) "
            "or pass -DEBUS_SDK_ROOT=/path/to/sdk. The devcontainer installs it from "
            "resource/ (see .devcontainer/Dockerfile).")
endif ()

add_library(amiga_ebus STATIC
        src/ebus/discovery.cpp
        src/ebus/env_bootstrap.cpp
        src/ebus/ip_config.cpp
        src/ebus/sdk_error.cpp
)

set_target_properties(amiga_ebus PROPERTIES
        CXX_EXTENSIONS OFF
)

# Warnings stay target-scoped; eBUS headers are SYSTEM via eBUS::eBUS.
target_compile_options(amiga_ebus PRIVATE -Wall -Wextra -Wpedantic)

# Compile-time GenICam env constants for src/ebus/env_bootstrap.cpp; values
# come from root cmake/FindeBUS.cmake (directory-scoped, hence set here).
target_compile_definitions(amiga_ebus PRIVATE
        AMIGA_EBUS_GENICAM_ENV_NAME="${EBUS_GENICAM_ENV_NAME}"
        AMIGA_EBUS_GENICAM_ROOT="${EBUS_GENICAM_ROOT_DIR}"
)

# RPATH to the SDK/GenICam lib dirs propagates from eBUS::eBUS
# (INTERFACE_LINK_OPTIONS, see cmake/FindeBUS.cmake) to every consumer.
target_link_libraries(amiga_ebus PUBLIC
        amiga_common
        eBUS::eBUS
)

# Tools for the web GUI (app/constants.py binds build/bin/<name>):
#   ebus_discover  GigE enumeration, --json for the Camera Tools scan table
#   ebus_set_ip    FORCEIP + persistent IP write, Camera Tools > Set IP
add_executable(ebus_discover src/ebus/discover_main.cpp)
add_executable(ebus_set_ip src/ebus/set_ip_main.cpp)
target_link_libraries(ebus_discover PRIVATE amiga_ebus)
target_link_libraries(ebus_set_ip PRIVATE amiga_ebus)
target_compile_options(ebus_discover PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(ebus_set_ip PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(ebus_discover ebus_set_ip PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
