################################################################################
# INS401 Driver
# This is a subdirectory of the top-level AmigaDrivers project.
# Shared dependencies (spdlog, yaml-cpp, Eigen) are provided by the parent.

# Build options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(INS401_BUILD_TESTS "Build test programs" ON)
else()
    option(INS401_BUILD_TESTS "Build test programs" OFF)
endif()


################################################################################
# Required System Packages (Threads provided by top-level)
find_package(Boost REQUIRED)


################################################################################
# Driver Library (shared code for main executable and tests)
set(LIB_SOURCES
        src/ins401_tool.cpp
        src/ins401_ethernet_socket.cpp
        src/ins401_discover.cpp
        src/ins401_receiver.cpp
        src/ins401_ntrip_client.cpp
        src/orientation_initializer.cpp
        src/initialization_monitor.cpp
        src/stationary_detector.cpp
        src/ins401_driver_app.cpp
)

# Header files
file(GLOB_RECURSE HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

# Create static library
add_library(ins401_lib STATIC ${LIB_SOURCES} ${HEADERS})

set_target_properties(ins401_lib PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

target_include_directories(ins401_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(ins401_lib PUBLIC
        # System libraries
        Threads::Threads
        # Boost
        Boost::boost
        # Eigen
        Eigen3::Eigen
        # Shared common library provides spdlog, yaml-cpp, Threads
        amiga_common
)


################################################################################
# Tests
if (INS401_BUILD_TESTS)
    add_subdirectory(tests)
endif ()


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   INS401_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Tests:      ${INS401_BUILD_TESTS}")
message(STATUS "----------------------------------------")
message(STATUS "")
