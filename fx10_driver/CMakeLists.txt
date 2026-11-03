# FX10-specific configuration; transport and storage live in ebus_driver.
find_package(eBUS REQUIRED)
add_library(fx10_config_core STATIC src/app_config.cpp src/apply_plan.cpp src/wavelengths.cpp)
target_include_directories(fx10_config_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fx10_config_core PUBLIC amiga_common)
target_compile_options(fx10_config_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(fx10_device_config STATIC src/ebus/camera_control.cpp ../ebus_driver/src/fx10_config.cpp)
target_include_directories(fx10_device_config PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/ebus_driver/include)
target_link_libraries(fx10_device_config PRIVATE fx10_config_core amiga_ebus eBUS::eBUS)
target_compile_options(fx10_device_config PRIVATE -Wall -Wextra -Wpedantic)

add_executable(fx10_snapshot tools/snapshot_main.cpp)
add_executable(fx10_reference tools/reference_main.cpp)
foreach(tool fx10_snapshot fx10_reference)
    target_include_directories(${tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${tool} PRIVATE ebus_camera fx10_config_core)
    set_target_properties(${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
add_subdirectory(tests)
