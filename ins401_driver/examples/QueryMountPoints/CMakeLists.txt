add_executable(QueryMountPoints query_mount_points.cpp)

target_link_libraries(QueryMountPoints PRIVATE ins401_lib)

target_compile_definitions(QueryMountPoints PRIVATE
        PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

set_target_properties(QueryMountPoints PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/examples
)
