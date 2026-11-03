# Shared SDK-buffer recorder and one acquisition lifecycle for all camera profiles.
find_package(eBUS REQUIRED)
add_library(ebus_raw STATIC src/raw_format.cpp src/raw_recorder.cpp)
target_include_directories(ebus_raw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ebus_raw PUBLIC amiga_common)
target_compile_options(ebus_raw PRIVATE -Wall -Wextra -Wpedantic)

add_library(ebus_camera STATIC src/camera_driver.cpp)
target_include_directories(ebus_camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ebus_camera PUBLIC ebus_raw amiga_common
    PRIVATE amiga_ebus eBUS::eBUS gox_device_config fx10_device_config)
target_compile_options(ebus_camera PRIVATE -Wall -Wextra -Wpedantic)

add_executable(ebus_raw_tests tests/test_raw.cpp tests/test_block_ids.cpp)
target_include_directories(ebus_raw_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/3rd_party)
target_link_libraries(ebus_raw_tests PRIVATE ebus_raw)
set_target_properties(ebus_raw_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)
add_test(NAME ebus.raw COMMAND ebus_raw_tests)
