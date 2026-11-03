# GOX-specific configuration; transport and storage live in ebus_driver.
find_package(eBUS REQUIRED)
add_library(gox_config_core STATIC
    src/app_config.cpp src/apply_plan.cpp src/buffering.cpp src/device_json.cpp
    src/ptp_status.cpp src/user_set.cpp)
target_include_directories(gox_config_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gox_config_core PUBLIC amiga_common)
target_compile_options(gox_config_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(gox_device_config STATIC
    src/ebus/camera_controller.cpp src/ebus/ptp_manager.cpp
    ../ebus_driver/src/gox_config.cpp)
# Do not expose ambiguous app_config.h / ebus/... names to other profiles.
target_include_directories(gox_device_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/ebus_driver/include)
target_link_libraries(gox_device_config PRIVATE gox_config_core amiga_ebus eBUS::eBUS)
target_compile_options(gox_device_config PRIVATE -Wall -Wextra -Wpedantic)

add_executable(jai_snapshot tools/snapshot_main.cpp)
target_include_directories(jai_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(jai_snapshot PRIVATE ebus_camera gox_config_core)
set_target_properties(jai_snapshot PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(GOX_TEST_SOURCES
    tests/test_apply_plan.cpp tests/test_buffering.cpp tests/test_config.cpp
    tests/test_device_json.cpp tests/test_ptp_status.cpp tests/test_templates.cpp
    tests/test_user_set.cpp)
add_executable(gox_tests tests/doctest_main.cpp ${GOX_TEST_SOURCES})
target_include_directories(gox_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(gox_tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/3rd_party)
target_link_libraries(gox_tests PRIVATE gox_config_core)
target_compile_definitions(gox_tests PRIVATE GOX_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config")
set_target_properties(gox_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)
foreach(test_source IN LISTS GOX_TEST_SOURCES)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_test(NAME gox.${test_name} COMMAND gox_tests --source-file=*${test_name}.cpp)
endforeach()
