################################################################################
# LMS4xxx Driver (SICK LMS4xxx 2D LiDAR, CoLa B over TCP)
# spdlog/yaml-cpp come via amiga_common, HDF5 via cmake/Dependencies.cmake.

################################################################################
# System packages
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)


################################################################################
# Driver Library
set(LIB_SOURCES
        src/driver.cpp
        src/tcp_client.cpp
        src/lms4xxx_driver_app.cpp
        src/scan_record.cpp
        src/scan_h5_file.cpp
        src/scan_record_writer.cpp
        src/app_config.cpp
        src/ntp_probe.cpp
        src/scan_verify.cpp
        src/frame_receiver.cpp
        src/cola_b.cpp
        src/command_builder.cpp
        src/scan_data_parser.cpp
)

file(GLOB_RECURSE HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
)

add_library(lms4xxx_lib STATIC ${LIB_SOURCES} ${HEADERS})

set_target_properties(lms4xxx_lib PROPERTIES
        CXX_EXTENSIONS OFF
)

target_include_directories(lms4xxx_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(lms4xxx_lib PUBLIC
        Threads::Threads
        Boost::boost # Asio
        amiga_common
)

# PRIVATE: no public header includes hdf5.h
target_link_libraries(lms4xxx_lib PRIVATE hdf5-static)


################################################################################
# Tests
if (AMIGA_BUILD_TESTS)
    add_subdirectory(tests)
endif ()


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   LMS4xxx_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Tests:      ${AMIGA_BUILD_TESTS}")
message(STATUS "----------------------------------------")
message(STATUS "")
