################################################################################
# common - Shared infrastructure library for all Amiga drivers
# Provides: logging, config loading, signal handling, SPSC ring buffer

find_package(Boost REQUIRED)  # header-only usage (Asio for tcp_client)

add_library(amiga_common STATIC
        src/activity_spinner.cpp
        src/logger.cpp
        src/signal_handler.cpp
        src/tcp_client.cpp
        src/utility.cpp
)

set_target_properties(amiga_common PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
)

target_include_directories(amiga_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(amiga_common PUBLIC
        Boost::boost
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        Threads::Threads
)


################################################################################
# Tests (doctest, vendored in 3rd_party/; Debug builds enable them by default)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(COMMON_BUILD_TESTS "Build common unit tests" ON)
else ()
    option(COMMON_BUILD_TESTS "Build common unit tests" OFF)
endif ()

if (COMMON_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
