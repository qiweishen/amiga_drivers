cmake_minimum_required(VERSION 3.12)
project(INS401_Driver
        DESCRIPTION "INS401 Driver"
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

# Build options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(INS401_BUILD_EXAMPLES "Build example programs" ON)
else()
    option(INS401_BUILD_EXAMPLES "Build example programs" OFF)
endif()


################################################################################
# Required System Packages
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

# 3rd_party libraries
add_subdirectory(3rd_party)

# FetchContent module
include(${PROJECT_SOURCE_DIR}/cmake/FetchContent.cmake)


################################################################################
# Driver Library (shared code for main executable and examples)
set(LIB_SOURCES
        src/tool.cpp
        src/ethernet_socket.cpp
        src/ins401_discover.cpp
        src/ins401_receiver.cpp
        src/ntrip_client.cpp
        src/orientation_initializer.cpp
        src/initialization_monitor.cpp
        src/stationary_detector.cpp
        src/terminal_spinner.cpp
)

# Header files
file(GLOB_RECURSE HEADERS
        ${PROJECT_SOURCE_DIR}/include/*.h
        ${PROJECT_SOURCE_DIR}/include/*.hpp
)

# Create static library
add_library(ins401_lib STATIC ${LIB_SOURCES} ${HEADERS})

set_target_properties(ins401_lib PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

target_include_directories(ins401_lib PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(ins401_lib PUBLIC
        # System libraries
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        # Boost
        Boost::boost
        # 3rd_party
        Eigen
        # FetchContent libraries
        spdlog::spdlog
        inih::inih
)


################################################################################
# Main Executable
add_executable(${PROJECT_NAME} main.cpp)

set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

target_link_libraries(${PROJECT_NAME} PRIVATE ins401_lib)


################################################################################
# Examples
if (INS401_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "========================================")
message(STATUS "   ${PROJECT_NAME} Configuration Summary")
message(STATUS "========================================")
message(STATUS "  Version:          ${PROJECT_VERSION}")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Install Prefix:   ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build Examples:   ${INS401_BUILD_EXAMPLES}")
message(STATUS "")
message(STATUS "  Dependencies:")
message(STATUS "    OpenSSL:        Found")
message(STATUS "    Boost:          Found")
message(STATUS "    Eigen3:         ${Eigen3_SOURCE_DIR}")
message(STATUS "    spdlog:         ${spdlog_SOURCE_DIR}")
message(STATUS "    inih:           ${inih_SOURCE_DIR}")
message(STATUS "========================================")
message(STATUS "")
