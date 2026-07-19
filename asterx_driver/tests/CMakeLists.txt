#
# Slim unit tests. Two suites:
#   asterx_test_commands    - command composition + geometry readback verifiers
#   asterx_test_app_config  - YAML config loading/validation
#
# The Septentrio SsnRx SDK itself is vendor-verified code and is not unit
# tested here; it is exercised end-to-end by the unified AmigaDrivers binary.
#

include(GoogleTest)

function(asterx_add_test name)
    cmake_parse_arguments(_T "" "" "SRCS;LIBS" ${ARGN})

    add_executable(${name} ${_T_SRCS})
    # Project headers come from asterx_lib's PUBLIC include/ directory.
    target_link_libraries(${name}
        PRIVATE
            asterx_lib
            GTest::gtest
            GTest::gtest_main
            ${_T_LIBS})

    target_compile_features(${name} PRIVATE cxx_std_17)

    gtest_discover_tests(${name}
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            LABELS unit)
endfunction()

asterx_add_test(asterx_test_commands
    SRCS test_commands.cpp)

asterx_add_test(asterx_test_app_config
    SRCS test_app_config.cpp)
