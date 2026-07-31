cmake_minimum_required(VERSION 3.14)
project(AmigaDrivers
        VERSION 0.1.0
        DESCRIPTION "Amiga Drivers - INS401, LMS4xxx, JAI Go-X, Septentrio AsteRx and Specim FX10"
        LANGUAGES CXX C
)


################################################################################
# Project Standards and Policies
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set default build type if not specified
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
    message(STATUS "Setting build type to '${CMAKE_BUILD_TYPE}' as none was specified.")
endif ()

# CMake policies
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
    set(CMAKE_POLICY_DEFAULT_CMP0167 OLD)
endif ()
if (POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
    set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)
endif ()
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)  # Allow our options to override subdirectory options


################################################################################
# Shared System Packages
find_package(Threads REQUIRED)
find_package(Eigen3 REQUIRED)


################################################################################
# Shared FetchContent libraries (spdlog, yaml-cpp, and nlohmann/json)
include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)

# Shared find modules (cmake/FindeBUS.cmake serves gox_driver and fx10_driver)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")


################################################################################
# Testing (ctest; driver test targets register via add_test / gtest_discover_tests)
enable_testing()

# GUI marker contract: driver_markers.h and app/services/markers.py must agree
# verbatim. The checker is stdlib-only, so the bare python3 interpreter works
# both in the build container and on the host.
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME contract_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_contracts.py)
endif ()


################################################################################
# Shared Common Library
add_subdirectory(common)


################################################################################
# Driver Subdirectories
add_subdirectory(ins401_driver)
add_subdirectory(lms4xxx_driver)
add_subdirectory(gox_driver)
add_subdirectory(asterx_driver)
# After asterx: in Debug its FetchContent googletest registers GTest::* first,
# so fx10's tests reuse it instead of the apt fallback
add_subdirectory(fx10_driver)


################################################################################
# DataConverter (standalone binary-to-CSV post-processing tool)
add_subdirectory(parse)


################################################################################
# Unified Executable (runs both drivers concurrently)
add_executable(AmigaDrivers main.cpp)

# Project version + git SHA, recorded once per run in <data_folder>/drivers.json
find_package(Git QUIET)
set(AMIGA_GIT_SHA "unknown")
if (GIT_FOUND)
    execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE AMIGA_GIT_SHA_RAW
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE AMIGA_GIT_SHA_RESULT)
    if (AMIGA_GIT_SHA_RESULT EQUAL 0)
        set(AMIGA_GIT_SHA "${AMIGA_GIT_SHA_RAW}")
    endif ()
endif ()
target_compile_definitions(AmigaDrivers PRIVATE
        AMIGA_DRIVERS_VERSION="${PROJECT_VERSION}"
        AMIGA_DRIVERS_GIT_SHA="${AMIGA_GIT_SHA}"
)

target_link_libraries(AmigaDrivers PRIVATE
        asterx_lib
        fx10_lib
        gox_lib
        ins401_lib
        lms4xxx_lib
)

set_target_properties(AmigaDrivers PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "========================================")
message(STATUS "   ${PROJECT_NAME} Configuration Summary")
message(STATUS "========================================")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Output Dir:       ${CMAKE_BINARY_DIR}/bin")
message(STATUS "")
message(STATUS "  Shared Dependencies:")
message(STATUS "    spdlog:         ${spdlog_SOURCE_DIR}")
message(STATUS "    yaml-cpp:       ${yaml-cpp_SOURCE_DIR}")
message(STATUS "    nlohmann/json:  ${nlohmann-json_SOURCE_DIR}")
message(STATUS "    doctest:        3rd_party/ (vendored)")
message(STATUS "========================================")
message(STATUS "")
