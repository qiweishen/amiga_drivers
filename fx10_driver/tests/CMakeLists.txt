# GoogleTest: prefer the distro CMake package; fall back to building the sources
# that Debian/Ubuntu's googletest package installs under /usr/src/googletest.
find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
  if(EXISTS "/usr/src/googletest/CMakeLists.txt")
    message(STATUS "GTest package config not found; building /usr/src/googletest sources")
    add_subdirectory(/usr/src/googletest "${CMAKE_BINARY_DIR}/_googletest" EXCLUDE_FROM_ALL)
    if(NOT TARGET GTest::gtest_main)
      add_library(GTest::gtest ALIAS gtest)
      add_library(GTest::gtest_main ALIAS gtest_main)
      add_library(GTest::gmock ALIAS gmock)
    endif()
  else()
    message(FATAL_ERROR
      "GoogleTest not found. Install it in the container: apt-get install libgtest-dev libgmock-dev")
  endif()
endif()

add_executable(fx10_tests
  test_accounting.cpp
  test_buffer_sizing.cpp
  test_pixel_format.cpp
  test_config.cpp
  test_envi_header.cpp
  test_envi_recorder.cpp
  test_sidecar.cpp
  test_wavelengths.cpp
)
target_link_libraries(fx10_tests PRIVATE fx10_core fx10_warnings GTest::gtest_main GTest::gmock
  nlohmann_json::nlohmann_json)
target_compile_definitions(fx10_tests PRIVATE
  FX10_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../config")

set_target_properties(fx10_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/tests)

include(GoogleTest)
gtest_discover_tests(fx10_tests)
