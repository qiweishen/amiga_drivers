################################################################################
# FetchContent Dependencies (Shared)
include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/3rd_party")
set(FETCHCONTENT_QUIET FALSE)


################################################################################
# HDF5 - Hierarchical data format version 5 library
FetchContent_Declare(
        hdf5
        GIT_REPOSITORY https://github.com/HDFGroup/hdf5.git
        GIT_TAG hdf5_1.14.6
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rd_party/FetchContent/hdf5
)
function(amiga_add_hdf5)
    find_package(ZLIB REQUIRED)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_TESTING OFF)
    set(HDF5_BUILD_TOOLS OFF)
    set(HDF5_BUILD_UTILS OFF)
    set(HDF5_BUILD_EXAMPLES OFF)
    set(HDF5_BUILD_HL_LIB OFF)
    set(HDF5_BUILD_CPP_LIB OFF)
    set(HDF5_ENABLE_Z_LIB_SUPPORT ON)   # gzip (deflate) filter on the channel datasets
    set(HDF5_ENABLE_SZIP_SUPPORT OFF)
    set(HDF5_ENABLE_THREADSAFE OFF)     # lms4xxx serializes its libhdf5 calls behind one process-wide mutex
    FetchContent_MakeAvailable(hdf5)
    set(hdf5_SOURCE_DIR "${hdf5_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
amiga_add_hdf5()


################################################################################
# spdlog - Fast C++ logging library
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rd_party/FetchContent/spdlog
)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog)


################################################################################
# yaml-cpp - YAML parser and emitter
FetchContent_Declare(
        yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG yaml-cpp-0.9.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rd_party/FetchContent/yaml-cpp
)
FetchContent_MakeAvailable(yaml-cpp)


################################################################################
# nlohmann/json - JSON for Modern C++ (drivers.json manifest, gox/fx10 tools)
FetchContent_Declare(
        nlohmann-json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rd_party/FetchContent/nlohmann-json
)
FetchContent_MakeAvailable(nlohmann-json)
