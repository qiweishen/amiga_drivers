################################################################################
# fx10 unit tests (doctest, SDK-free)

set(FX10_TEST_SOURCES
        test_accounting.cpp
        test_apply_plan.cpp
        test_buffer_sizing.cpp
        test_config.cpp
        test_envi_header.cpp
        test_envi_recorder.cpp
        test_pixel_format.cpp
        test_recording_pipeline.cpp
        test_session_metadata.cpp
        test_sensor_trigger_stall.cpp
        test_stats_line.cpp
        test_wavelengths.cpp
)

add_executable(fx10_tests
        doctest_main.cpp
        ${FX10_TEST_SOURCES}
)

target_link_libraries(fx10_tests PRIVATE fx10_core fx10_warnings)
target_include_directories(fx10_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/3rd_party)
target_compile_definitions(fx10_tests PRIVATE
        FX10_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../config")

set_target_properties(fx10_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)

# One ctest entry per source file (doctest --source-file filter)
foreach (test_source IN LISTS FX10_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_test(NAME fx10.${test_name}
            COMMAND fx10_tests --source-file=*${test_source})
endforeach ()
