cmake_minimum_required(VERSION 3.18)
project(AmigaDrivers
        VERSION 0.1.0
        DESCRIPTION "Amiga Drivers - Septentrio AsteRx, Specim FX10e, JAI Go-X, and SICK LMS4xxx"
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
# Build boundaries (defaults preserve the complete rig)
option(AMIGA_BUILD_TESTS "Build unit tests" ON)
option(AMIGA_ENABLE_EBUS "Build eBUS camera acquisition and device tools" ON)
option(AMIGA_ENABLE_ASTERX "Build AsteRx acquisition and its Qt/ssnrx SDK" ON)
option(AMIGA_ENABLE_LMS4XXX "Build LMS acquisition and its HDF5 recorder" ON)
# Caller-provided application identity; no version-control query for metadata.
set(AMIGA_BUILD_REVISION "unknown" CACHE STRING "Source revision recorded in drivers.json")
if (NOT AMIGA_BUILD_REVISION MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR "AMIGA_BUILD_REVISION must contain only letters, digits, '.', '_', '+' or '-'")
endif ()

################################################################################
# Shared System Packages
find_package(Threads REQUIRED)


################################################################################
# Shared FetchContent libraries (sensor_trigger, HDF5, spdlog, yaml-cpp, and nlohmann/json)
include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)

# Shared find modules (cmake/FindeBUS.cmake serves gox_driver and fx10_driver)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")


################################################################################
# Build Testings
if (AMIGA_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_FOUND)
        add_test(NAME contract_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/check_contracts.py)
    endif ()
endif ()

################################################################################
# Shared Common Library
add_subdirectory(common)


################################################################################
# Driver Subdirectories
if (AMIGA_ENABLE_LMS4XXX)
    add_subdirectory(lms4xxx_driver)
endif ()
add_subdirectory(gox_driver)
if (AMIGA_ENABLE_ASTERX)
    add_subdirectory(asterx_driver)
endif ()
add_subdirectory(fx10_driver)


################################################################################
# Unified executable: composition depends only on enabled hardware adapters.
# Camera format/recorder cores and their tests remain available with eBUS off.
add_executable(AmigaDrivers main.cpp acquisition.cpp)
target_compile_definitions(AmigaDrivers PRIVATE
        AMIGA_DRIVERS_VERSION="${PROJECT_VERSION}"
        AMIGA_DRIVERS_GIT_SHA="${AMIGA_BUILD_REVISION}"
        AMIGA_ENABLE_EBUS=$<BOOL:${AMIGA_ENABLE_EBUS}>
        AMIGA_ENABLE_ASTERX=$<BOOL:${AMIGA_ENABLE_ASTERX}>
        AMIGA_ENABLE_LMS4XXX=$<BOOL:${AMIGA_ENABLE_LMS4XXX}>
)
target_link_libraries(AmigaDrivers PRIVATE amiga_common)
if (AMIGA_ENABLE_ASTERX)
    target_link_libraries(AmigaDrivers PRIVATE asterx_lib)
endif ()
if (AMIGA_ENABLE_LMS4XXX)
    target_link_libraries(AmigaDrivers PRIVATE lms4xxx_lib)
endif ()
if (AMIGA_ENABLE_EBUS)
    target_link_libraries(AmigaDrivers PRIVATE fx10_lib gox_lib)
endif ()

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
message(STATUS "    sensor_trigger: ${sensor_trigger_SOURCE_DIR}")
message(STATUS "    hdf5:           ${hdf5_SOURCE_DIR}")
message(STATUS "    spdlog:         ${spdlog_SOURCE_DIR}")
message(STATUS "    yaml-cpp:       ${yaml-cpp_SOURCE_DIR}")
message(STATUS "    nlohmann/json:  ${nlohmann-json_SOURCE_DIR}")
message(STATUS "    doctest:        3rd_party/ (vendored)")
message(STATUS "========================================")
message(STATUS "")
