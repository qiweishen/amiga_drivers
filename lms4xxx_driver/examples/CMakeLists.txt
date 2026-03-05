################################################################################
# LMS4xxx Driver Examples

add_executable(lms4xxx_basic_scan basic_scan.cpp)

target_link_libraries(lms4xxx_basic_scan PRIVATE
        lms4xxx_lib
)

set_target_properties(lms4xxx_basic_scan PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/examples
)
