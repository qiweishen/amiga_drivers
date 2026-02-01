cmake_minimum_required(VERSION 3.12)
project(LMS41xxx_Driver
        DESCRIPTION "LMS41xxx_Driver"
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


################################################################################
# External project configuration
set(SICK_SCAN_XD_ROOT_DIR "${PROJECT_SOURCE_DIR}/3rd_party/FetchContent/sick_scan_xd" CACHE PATH "sick_scan_xd source directory")
include(${PROJECT_SOURCE_DIR}/cmake/FetchContent.cmake)


################################################################################
# Project Configuration
set(SOURCES main.cpp ${SICK_SCAN_XD_ROOT_DIR}/test/src/sick_scan_xd_api/sick_scan_xd_api_wrapper.c
        src/tool.cpp)
add_executable(${PROJECT_NAME} ${SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC "${SICK_SCAN_XD_ROOT_DIR}/include") # include folder for sick_scan_api.h
target_link_libraries(${PROJECT_NAME} PUBLIC dl)  # dynamic library loading
add_dependencies(${PROJECT_NAME} sick_scan_xd_build)
