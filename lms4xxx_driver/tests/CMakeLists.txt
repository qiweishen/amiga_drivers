################################################################################
# lms4xxx unit tests (doctest, device-free)

set(LMS4XXX_TEST_SOURCES
        test_command_builder.cpp
        test_clock_quality.cpp
        test_config_schema.cpp
        test_frame_receiver.cpp
        test_ntp_probe.cpp
        test_recording_status.cpp
        test_statistics.cpp
        test_scan_data_parser.cpp
        test_scan_h5_file.cpp
        test_scan_record.cpp
        test_scan_record_writer.cpp
        test_scan_verify.cpp
)

add_executable(lms4xxx_tests
        doctest_main.cpp
        ${LMS4XXX_TEST_SOURCES}
)

# The tests include hdf5.h themselves to read the files back
target_link_libraries(lms4xxx_tests PRIVATE lms4xxx_lib hdf5-static)

# SYSTEM: no warnings from the vendored doctest header
target_include_directories(lms4xxx_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/3rd_party)

# The shipped template is asserted against the loader, so a config that documents
# a key the schema rejects fails the build's tests rather than the field.
target_compile_definitions(lms4xxx_tests PRIVATE
        LMS4XXX_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../config")

set_target_properties(lms4xxx_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)

# One ctest entry per source file (doctest's --source-file filter) instead of a
# single opaque entry: a failure then names the area, and the files run in
# parallel under `ctest -j`. No test-name list to keep in sync.
foreach (test_source IN LISTS LMS4XXX_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_test(NAME lms4xxx.${test_name}
            COMMAND lms4xxx_tests --source-file=*${test_source})
endforeach ()
