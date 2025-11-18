cmake_minimum_required(VERSION 3.15...3.25)
project(INS401_Driver LANGUAGES CXX C)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------------------

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

# ---------------------------------------------------------------------------------------

include(FetchContent)

# For formatting strings
FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt
        GIT_TAG 12.1.0
)
FetchContent_MakeAvailable(fmt)

# For INI configure file parsing
FetchContent_Declare(
        inih
        GIT_REPOSITORY https://github.com/benhoyt/inih
        GIT_TAG r62
)
FetchContent_GetProperties(inih)
if (NOT inih_POPULATED)
    FetchContent_MakeAvailable(inih)
    add_library(inih STATIC
            ${inih_SOURCE_DIR}/ini.c
            ${inih_SOURCE_DIR}/cpp/INIReader.cpp
    )
    target_include_directories(inih PUBLIC
            ${inih_SOURCE_DIR}
            ${inih_SOURCE_DIR}/cpp
    )
endif ()

# For logging system
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog
        GIT_TAG v1.16.0
)
FetchContent_MakeAvailable(spdlog)

# ---------------------------------------------------------------------------------------

include_directories(
        ${PROJECT_SOURCE_DIR}/include
        ${Boost_INCLUDE_DIRS}
)


set(SOURCES
        src/tool.cpp
        src/ins_discover.cpp
        src/ins_receiver.cpp
        src/ntrip_client.cpp
        main.cpp
)


add_executable(${PROJECT_NAME} ${SOURCES})


target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        Boost::boost
        fmt::fmt
        inih
        spdlog::spdlog
)
