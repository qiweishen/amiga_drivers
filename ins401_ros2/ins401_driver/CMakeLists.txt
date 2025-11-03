cmake_minimum_required(VERSION 3.22...3.25)
project(INS401_Driver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(
        ${PROJECT_SOURCE_DIR}/include
)

set(SOURCES
        ${PROJECT_SOURCE_DIR}/src/tool.cpp
        ${PROJECT_SOURCE_DIR}/src/ins_device_finder.cpp
        ${PROJECT_SOURCE_DIR}/src/ins_driver.cpp
        ${PROJECT_SOURCE_DIR}/main.cpp
        include/ins_device_finder.h
        include/ins_driver.h
        src/ntrip_client.cpp
        src/ntrip_client.h
)

add_executable(${PROJECT_NAME} ${SOURCES})

find_package(Threads REQUIRED)
target_link_libraries(INS401_Driver Threads::Threads)
