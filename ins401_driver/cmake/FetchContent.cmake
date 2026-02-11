################################################################################
# FetchContent Dependencies
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)


################################################################################
# Eigen - C++ template library for linear algebra
FetchContent_Declare(
        eigen
        GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
        GIT_TAG 3.4.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/FetchContent/eigen
)
FetchContent_MakeAvailable(eigen)


################################################################################
# inih - INI file parser
FetchContent_Declare(
        inih
        GIT_REPOSITORY https://github.com/benhoyt/inih
        GIT_TAG r62
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/FetchContent/inih
)

FetchContent_GetProperties(inih)
if (NOT inih_POPULATED)
    FetchContent_Populate(inih)
    # Create inih library manually since it doesn't provide CMakeLists.txt
    add_library(inih STATIC
            ${inih_SOURCE_DIR}/ini.c
            ${inih_SOURCE_DIR}/cpp/INIReader.cpp
    )
    target_include_directories(inih PUBLIC
            $<BUILD_INTERFACE:${inih_SOURCE_DIR}>
            $<BUILD_INTERFACE:${inih_SOURCE_DIR}/cpp>
            $<INSTALL_INTERFACE:include>
    )
    # Set C++ standard for INIReader.cpp
    set_target_properties(inih PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
    )
    if (NOT TARGET inih::inih)
        add_library(inih::inih ALIAS inih)
    endif ()
endif ()


################################################################################
# spdlog - Fast C++ logging library
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog
        GIT_TAG v1.16.0
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/FetchContent/spdlog
)
FetchContent_MakeAvailable(spdlog)
