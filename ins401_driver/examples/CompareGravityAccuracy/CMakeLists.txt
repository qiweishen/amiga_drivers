# FetchContent module
################################################################################
# FetchContent Dependencies
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)


################################################################################
# GeographicLib - small C++ library for geodesic calculations
FetchContent_Declare(
        geographiclib
        GIT_REPOSITORY https://github.com/geographiclib/geographiclib.git
        GIT_TAG v2.7
        GIT_SHALLOW TRUE
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/FetchContent/geographiclib
)
FetchContent_MakeAvailable(geographiclib)

################################################################################
add_executable(CompareGravityAccuracy compare_gravity_accuracy.cpp)

target_link_libraries(CompareGravityAccuracy PRIVATE ins401_lib GeographicLib)

target_compile_definitions(CompareGravityAccuracy PRIVATE
        PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

set_target_properties(CompareGravityAccuracy PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/examples
)
