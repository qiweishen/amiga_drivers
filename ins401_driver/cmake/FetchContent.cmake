################################################################################
# FetchContent Dependencies
include(FetchContent)

set(FETCHCONTENT_QUIET FALSE)


#################################################################################
## libtins - Network packet sniffing and crafting
#FetchContent_Declare(
#        tins
#        GIT_REPOSITORY https://github.com/mfontanini/libtins
#        GIT_TAG v4.5
#        GIT_SHALLOW TRUE
#        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/FetchContent/tins
#)
## Configure libtins build options before making it available
#set(LIBTINS_BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
#set(LIBTINS_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
#set(LIBTINS_ENABLE_CXX11 ON CACHE BOOL "Compile libtins with c++11 features" FORCE)
## Handle libtins' old CMake version requirements
#if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
#    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Fix libtins old cmake version" FORCE)
#endif ()
#FetchContent_MakeAvailable(tins)
## Clean up policy setting
#if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
#    unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
#endif ()
## Fix libtins include directories and create modern alias
#if (TARGET tins)
#    if (NOT TARGET tins::tins)
#        add_library(tins::tins ALIAS tins)
#    endif ()
#    # Ensure include directories are properly set
#    get_target_property(TINS_INC_DIRS tins INTERFACE_INCLUDE_DIRECTORIES)
#    if (NOT TINS_INC_DIRS)
#        target_include_directories(tins PUBLIC
#                $<BUILD_INTERFACE:${tins_SOURCE_DIR}/include>
#                $<INSTALL_INTERFACE:include>
#        )
#    endif ()
#endif ()


#################################################################################
## fmt - Modern formatting library
#FetchContent_Declare(
#        fmt
#        GIT_REPOSITORY https://github.com/fmtlib/fmt
#        GIT_TAG 12.1.0
#        GIT_SHALLOW TRUE
#        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/FetchContent/fmt
#)
#FetchContent_MakeAvailable(fmt)

################################################################################
# inih - INI file parser
FetchContent_Declare(
        inih
        GIT_REPOSITORY https://github.com/benhoyt/inih
        GIT_TAG r62
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/FetchContent/inih
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
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/FetchContent/spdlog
)
FetchContent_MakeAvailable(spdlog)
