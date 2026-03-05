cmake_minimum_required(VERSION 3.14)
project(AmigaDrivers
        DESCRIPTION "Amiga Drivers - INS401 and LMS41xxx"
        LANGUAGES CXX C
)


################################################################################
# Project Standards and Policies
set(CMAKE_CXX_STANDARD 17)
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
# Shared FetchContent libraries (spdlog, yaml-cpp and sick_scan_xd)
include(${CMAKE_SOURCE_DIR}/cmake/FetchContent.cmake)


################################################################################
# Shared Common Library
add_subdirectory(common)


################################################################################
# Driver Subdirectories
add_subdirectory(ins401_driver)
add_subdirectory(lms4xxx_driver)


################################################################################
# DataConverter (standalone binary-to-CSV post-processing tool)
add_subdirectory(app)


################################################################################
# Unified Executable (runs both drivers concurrently)
add_executable(AmigaDrivers main.cpp)

target_link_libraries(AmigaDrivers PRIVATE
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
message(STATUS "")
message(STATUS "  Shared Dependencies:")
message(STATUS "    spdlog:         ${spdlog_SOURCE_DIR}")
message(STATUS "    yaml-cpp:       ${yaml-cpp_SOURCE_DIR}")
message(STATUS "========================================")
message(STATUS "")
