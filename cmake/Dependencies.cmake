################################################################################
# FetchContent Dependencies (Shared)
include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/3rd_party")
set(FETCHCONTENT_QUIET FALSE)


################################################################################
# SensorSync host API follows upstream main without a version/content pin.
# There is no CMakeLists.txt, so MakeAvailable only populates the sources.
# amiga_common includes session_client.cpp; board firmware is not built here.
FetchContent_Declare(
        sensor_trigger
        GIT_REPOSITORY https://github.com/qiweishen/sensor_trigger.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rd_party/External/sensor_trigger
)
FetchContent_MakeAvailable(sensor_trigger)


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
