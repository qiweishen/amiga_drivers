################################################################################
# LMS4xxx Driver
# Direct TCP/CoLa B protocol driver for SICK LMS4xxx 2D LiDAR sensors.
# This is a subdirectory of the top-level AmigaDrivers project.
# Shared dependencies (spdlog, yaml-cpp) are provided by amiga_common.

# Build options
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(LMS4XXX_BUILD_EXAMPLES "Build example programs" ON)
else()
    option(LMS4XXX_BUILD_EXAMPLES "Build example programs" OFF)
endif()


################################################################################
# Required System Packages (Threads provided by top-level)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)


################################################################################
# Driver Library
set(LIB_SOURCES
        src/lms4xxx_driver.cpp
        src/lms4xxx_driver_app.cpp
        src/lms4xxx_scan_record_writer.cpp
        src/lms4xxx_tool.cpp
        src/lms4xxx_tcp_client.cpp
        src/lms4xxx_frame_receiver.cpp
        src/lms4xxx_cola_b.cpp
        src/lms4xxx_command_builder.cpp
        src/lms4xxx_scan_data_parser.cpp
)

# Header files
file(GLOB_RECURSE HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

# Create static library
add_library(lms4xxx_lib STATIC ${LIB_SOURCES} ${HEADERS})

set_target_properties(lms4xxx_lib PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

target_include_directories(lms4xxx_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(lms4xxx_lib PUBLIC
        # System libraries
        Threads::Threads
        # Boost (Asio for TCP networking)
        Boost::boost
        # Shared common library provides spdlog, yaml-cpp, RingBuffer
        amiga_common
)


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   LMS4xxx_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Examples:   ${LMS4XXX_BUILD_EXAMPLES}")
message(STATUS "----------------------------------------")
message(STATUS "")
