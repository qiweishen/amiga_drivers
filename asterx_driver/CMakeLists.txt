################################################################################
# AsteRx-i3 D Pro+ Driver (Septentrio raw IMU + dual-antenna GNSS recorder)
# This is a subdirectory of the top-level AmigaDrivers project.
# spdlog (compiled), yaml-cpp and Threads are provided by amiga_common.
# Qt5 (Core, Network, SerialPort) is a hard dependency of the vendored
# Septentrio SsnRx SDK — the devcontainer provides it via qtbase5-dev +
# libqt5serialport5-dev. GoogleTest is fetched here for unit tests only.

# Build options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(ASTERX_BUILD_TESTS "Build asterx unit tests" ON)
else ()
    option(ASTERX_BUILD_TESTS "Build asterx unit tests" OFF)
endif ()


################################################################################
# Required System Packages (Threads provided by top-level)
# QtCore/QtNetwork are used by the driver; QtSerialPort only by ssnrx.
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Network SerialPort)


################################################################################
# ssnrx - Septentrio "GNSS Receiver Communication SDK for C++/Qt" (vendored)
set(SSNRX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/RxTools/ssnrx")

add_library(ssnrx STATIC
        "${SSNRX_DIR}/src/ssnrx.cpp"
        "${SSNRX_DIR}/src/crc.c"
        # Q_OBJECT header: listed as a source so AUTOMOC scans it
        "${SSNRX_DIR}/include/ssnrx.h"
)
set_target_properties(ssnrx PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        AUTOMOC ON
        POSITION_INDEPENDENT_CODE ON
)
target_include_directories(ssnrx SYSTEM PUBLIC "${SSNRX_DIR}/include")
target_link_libraries(ssnrx PUBLIC
        Qt5::Core
        Qt5::Network
        Qt5::SerialPort
)
# Vendor code is exempt from the project warning policy
target_compile_options(ssnrx PRIVATE -w)


################################################################################
# Driver Library (consumed by the unified AmigaDrivers executable and tests)
add_library(asterx_lib STATIC
        src/app_config.cpp
        src/commands.cpp
        src/sbf_writer.cpp
        src/session.cpp
        src/asterx_driver_app.cpp
        # Q_OBJECT headers live in include/, away from their .cpp files:
        # AUTOMOC only auto-discovers same-directory headers, so list them
        include/session.hpp
)
set_target_properties(asterx_lib PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        AUTOMOC ON
        POSITION_INDEPENDENT_CODE ON
)
target_include_directories(asterx_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(asterx_lib PUBLIC
        # Septentrio SDK (provides Qt5::Network/SerialPort transitively)
        ssnrx
        # session.hpp exposes QObject/QTimer in its public interface
        Qt5::Core
        # Shared common library provides spdlog, yaml-cpp, Threads
        amiga_common
)


################################################################################
# Tests (GoogleTest via FetchContent, hermetic;
# enable_testing() is called at the top level)
if (ASTERX_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
            GIT_SHALLOW TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif ()


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   AsteRx_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  Qt5:              ${Qt5_VERSION} (${Qt5_DIR})")
message(STATUS "  Build Tests:      ${ASTERX_BUILD_TESTS}")
message(STATUS "----------------------------------------")
message(STATUS "")
