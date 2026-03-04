################################################################################
# common - Shared infrastructure library for all Amiga drivers
# Provides: logging, config loading, binary I/O, thread-safe queue, signal handling

add_library(amiga_common STATIC
        src/activity_spinner.cpp
        src/binary_writer.cpp
        src/signal_handler.cpp
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
        spdlog::spdlog
        yaml-cpp::yaml-cpp
        Threads::Threads
)
