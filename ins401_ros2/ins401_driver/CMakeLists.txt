cmake_minimum_required(VERSION 3.22...3.25)
project(INS401_Driver LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)


add_subdirectory(deps)


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


target_link_libraries(${PROJECT_NAME}
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        Boost::boost
)
