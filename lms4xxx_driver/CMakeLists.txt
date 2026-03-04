################################################################################
# LMS4xxx Driver
# This is a subdirectory of the top-level AmigaDrivers project.


################################################################################
# Driver Library (shared code for main executable and unified executable)
set(LIB_SOURCES
        src/buffered_writer.cpp
        src/csv_converter.cpp
        src/lidar_driver_app.cpp
        src/lidar_receiver.cpp
        src/lidar_tool.cpp
        src/point_cloud_callback.cpp
        ${SICK_SCAN_XD_ROOT_DIR}/test/src/sick_scan_xd_api/sick_scan_xd_api_wrapper.c
)

add_library(lms4xxx_lib STATIC ${LIB_SOURCES})

target_include_directories(lms4xxx_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SICK_SCAN_XD_ROOT_DIR}/include
)

target_link_libraries(lms4xxx_lib PUBLIC
        dl
        # Shared common library provides spdlog, yaml-cpp, Threads
        amiga_common
)

add_dependencies(lms4xxx_lib sick_scan_xd_build)


################################################################################
# Status Messages
message(STATUS "")
message(STATUS "----------------------------------------")
message(STATUS "   LMS4xxx_Driver Configuration")
message(STATUS "----------------------------------------")
message(STATUS "  Build Type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  sick_scan_xd:     ${SICK_SCAN_XD_ROOT_DIR}")
message(STATUS "----------------------------------------")
message(STATUS "")
