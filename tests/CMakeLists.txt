# Unit tests for the pure, OBS-free parts of the plugin (src/buffer-fit.*,
# src/encoder-check.*, and since v0.6.0 src/frame-arena.* and
# src/frame-ring.*). No libobs, no Qt, no FFmpeg, no test framework. Two ways
# to build them:
#
# 1. From the plugin's own CMakeLists.txt (same compiler flags as the plugin):
#      cmake --preset <preset> -DTRIGGLOW_BUILD_TESTS=ON
#      cmake --build <dir> --config RelWithDebInfo
#      ctest --test-dir <dir> -C RelWithDebInfo --output-on-failure
#
# 2. Standalone, without downloading any OBS dependency (what CI's unit-tests
#    job in .github/workflows/build-project.yaml does):
#      cmake -S tests -B build_tests
#      cmake --build build_tests --config RelWithDebInfo
#      ctest --test-dir build_tests -C RelWithDebInfo --output-on-failure

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.28...3.30)

  # Same default as cmake/common/bootstrap.cmake for single-config generators:
  # optimized with NDEBUG, like the shipped plugin (the tests don't use
  # assert()). Set before project() so it also wins over MSVC's Debug default.
  get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(NOT _multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type")
  endif()

  project(obs-trigglow-dynamic-delay-tests LANGUAGES CXX)
  enable_testing()
endif()

set(_src "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(buffer_fit_tests buffer_fit_tests.cpp "${_src}/buffer-fit.cpp" "${_src}/buffer-fit.hpp")
target_include_directories(buffer_fit_tests PRIVATE "${_src}")
set_target_properties(buffer_fit_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME buffer_fit_tests COMMAND buffer_fit_tests)

# Links buffer-fit too: the tests size the capped and uncapped buffers with
# bufferfit::Compute(), like BufferModeController does.
add_executable(
  encoder_check_tests
  encoder_check_tests.cpp
  "${_src}/encoder-check.cpp"
  "${_src}/encoder-check.hpp"
  "${_src}/buffer-fit.cpp"
  "${_src}/buffer-fit.hpp"
)
target_include_directories(encoder_check_tests PRIVATE "${_src}")
set_target_properties(encoder_check_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME encoder_check_tests COMMAND encoder_check_tests)

# v0.6.0 codec workers: the payload arena and the ring's slot state machine.
find_package(Threads REQUIRED)

add_executable(frame_arena_tests frame_arena_tests.cpp check.hpp "${_src}/frame-arena.cpp" "${_src}/frame-arena.hpp")
target_include_directories(frame_arena_tests PRIVATE "${_src}")
target_link_libraries(frame_arena_tests PRIVATE Threads::Threads)
set_target_properties(frame_arena_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME frame_arena_tests COMMAND frame_arena_tests)

add_executable(
  frame_ring_tests
  frame_ring_tests.cpp
  check.hpp
  "${_src}/frame-ring.cpp"
  "${_src}/frame-ring.hpp"
  "${_src}/frame-arena.cpp"
  "${_src}/frame-arena.hpp"
)
target_include_directories(frame_ring_tests PRIVATE "${_src}")
set_target_properties(frame_ring_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME frame_ring_tests COMMAND frame_ring_tests)
