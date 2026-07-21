################################################################################
# common unit tests (doctest; enable_testing() is called at the top level)

add_executable(common_tests
        doctest_main.cpp
        test_driver_app.cpp
        test_file_writer.cpp
        test_logger.cpp
        test_util.cpp
)

target_link_libraries(common_tests PRIVATE amiga_common)

# SYSTEM: keep warnings from firing inside the vendored doctest header
target_include_directories(common_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/3rd_party)

set_target_properties(common_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests
)

add_test(NAME common_tests COMMAND common_tests)
