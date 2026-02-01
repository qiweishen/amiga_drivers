################################################################################
# External Dependencies
include(FetchContent)
include(ExternalProject)
include(ProcessorCount)
set(FETCHCONTENT_QUIET FALSE)


################################################################################
# sick_scan_xd - Driver and tools for SICK LiDAR and RADAR devices
set(SICK_SCAN_XD_ROOT_DIR "${PROJECT_SOURCE_DIR}/3rd_party/FetchContent/sick_scan_xd" CACHE PATH "sick_scan_xd source directory")
set(SICK_SCAN_XD_BINARY_DIR "${SICK_SCAN_XD_ROOT_DIR}/build" CACHE PATH "sick_scan_xd build directory")

if (NOT EXISTS "${SICK_SCAN_XD_ROOT_DIR}/CMakeLists.txt")
    FetchContent_Declare(
            sick_scan_xd
            GIT_REPOSITORY https://github.com/SICKAG/sick_scan_xd.git
            GIT_TAG 3.8.0
            GIT_SHALLOW TRUE
            SOURCE_DIR ${SICK_SCAN_XD_ROOT_DIR}
    )
    FetchContent_Populate(sick_scan_xd)
endif ()

ProcessorCount(SICK_SCAN_XD_BUILD_JOBS)
if (SICK_SCAN_XD_BUILD_JOBS EQUAL 0)
    set(SICK_SCAN_XD_BUILD_JOBS 4)
endif ()

ExternalProject_Add(
        sick_scan_xd_build
        SOURCE_DIR ${SICK_SCAN_XD_ROOT_DIR}
        BINARY_DIR ${SICK_SCAN_XD_BINARY_DIR}
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        PATCH_COMMAND ""
        CMAKE_ARGS
        -DROS_VERSION=0
        -DLDMRS=0
        -DSCANSEGMENT_XD=0
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> -- -j${SICK_SCAN_XD_BUILD_JOBS}
        INSTALL_COMMAND ""
)
