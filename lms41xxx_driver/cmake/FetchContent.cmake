################################################################################
# FetchContent Dependencies
include(FetchContent)
include(ExternalProject)
include(ProcessorCount)
set(FETCHCONTENT_QUIET FALSE)


################################################################################
# sick_scan_xd - Driver and tools for SICK LiDAR and RADAR devices
FetchContent_Declare(
        sick_scan_xd
        GIT_REPOSITORY https://github.com/SICKAG/sick_scan_xd.git
        GIT_TAG 3.8.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/FetchContent/sick_scan_xd
)

FetchContent_MakeAvailable(sick_scan_xd)
FetchContent_GetProperties(sick_scan_xd)

set(SICK_SCAN_XD_ROOT_DIR "${sick_scan_xd_SOURCE_DIR}" CACHE PATH "sick_scan_xd source directory")
set(SICK_SCAN_XD_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/sick_scan_xd-build" CACHE PATH "sick_scan_xd build directory")

ProcessorCount(SICK_SCAN_XD_BUILD_JOBS)
if (SICK_SCAN_XD_BUILD_JOBS EQUAL 0)
    set(SICK_SCAN_XD_BUILD_JOBS 1)
endif ()

ExternalProject_Add(
        sick_scan_xd_ep
        SOURCE_DIR ${sick_scan_xd_SOURCE_DIR}
        BINARY_DIR ${SICK_SCAN_XD_BINARY_DIR}
        CMAKE_ARGS
        -DROS_VERSION=0
        -DLDMRS=0
        -DSCANSEGMENT_XD=0
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> -- -j${SICK_SCAN_XD_BUILD_JOBS}
        INSTALL_COMMAND ""
)
