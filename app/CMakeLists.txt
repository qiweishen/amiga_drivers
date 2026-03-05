################################################################################
# DataConverter - standalone binary-to-CSV post-processing tool
################################################################################

add_executable(DataConverter
        src/main.cpp
        src/lms4xxx_csv_converter.cpp
        src/ins401_csv_converter.cpp
)

target_include_directories(DataConverter PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(DataConverter PRIVATE
        spdlog::spdlog
)

set_target_properties(DataConverter PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
