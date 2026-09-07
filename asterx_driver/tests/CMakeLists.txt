################################################################################
# asterx unit tests (doctest, device-free; the session tests use a fake receiver on localhost)

set(ASTERX_TEST_SOURCES
        test_app_config.cpp
        test_commands.cpp
        test_sbf_live_recorder.cpp
        test_sbf_parsers.cpp
        test_sbf_write_queue.cpp
        test_sbf_writer.cpp
        test_session.cpp
        test_warmup_gate.cpp
)

add_executable(asterx_tests
        doctest_main.cpp
        ${ASTERX_TEST_SOURCES}
)

# The session test builds SBF frames with the vendored CRC helper (SsnRx src)
target_link_libraries(asterx_tests PRIVATE asterx_lib Qt5::Core Qt5::Network)
target_include_directories(asterx_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/3rd_party)
target_include_directories(asterx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${SSNRX_DIR}/src")
target_compile_definitions(asterx_tests PRIVATE
        ASTERX_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../config")

set_target_properties(asterx_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)

# One ctest entry per source file (doctest --source-file filter)
foreach (test_source IN LISTS ASTERX_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_test(NAME asterx.${test_name}
            COMMAND asterx_tests --source-file=*${test_source})
endforeach ()
