# Offline gravity-model comparison tool. Opt-in: it FetchContents GeographicLib,
# whose own CMake registers ~200 tool tests (plus 4 never-built EXCLUDE_FROM_ALL
# test programs) into ctest and would pollute the project test run.
option(INS401_BUILD_GRAVITY_COMPARE "Build CompareGravityAccuracy (fetches GeographicLib)" OFF)
if (INS401_BUILD_GRAVITY_COMPARE)
    add_subdirectory(CompareGravityAccuracy)
endif ()
