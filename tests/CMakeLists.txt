# Unit tests for the pure, OBS-free parts of the plugin (src/buffer-fit.*,
# src/encoder-check.*, and since v0.6.0 src/ui-tokens.*, src/auto-names.*,
# src/overlay-layout.*, src/frame-arena.*, src/frame-ring.*,
# src/degradation-policy.* and a stress test of src/codec-pipeline.*). No
# libobs, no Qt, no FFmpeg, no test framework. Two ways to build them:
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

# Color tokens of the dock and the report dialog: the WCAG AA contract on the
# palettes of OBS's themes (src/ui-tokens.hpp).
add_executable(ui_tokens_tests ui_tokens_tests.cpp "${_src}/ui-tokens.cpp" "${_src}/ui-tokens.hpp")
target_include_directories(ui_tokens_tests PRIVATE "${_src}")
set_target_properties(ui_tokens_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME ui_tokens_tests COMMAND ui_tokens_tests)

# Names of the objects the plugin manages and their migration from the
# "(no tocar)" names of v0.5.1 and earlier (src/auto-names.hpp).
add_executable(auto_names_tests auto_names_tests.cpp "${_src}/auto-names.cpp" "${_src}/auto-names.hpp")
target_include_directories(auto_names_tests PRIVATE "${_src}")
set_target_properties(auto_names_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME auto_names_tests COMMAND auto_names_tests)

# Placement of the live overlay badge and its file names (src/overlay-layout.hpp).
add_executable(overlay_layout_tests overlay_layout_tests.cpp "${_src}/overlay-layout.cpp" "${_src}/overlay-layout.hpp")
target_include_directories(overlay_layout_tests PRIVATE "${_src}")
set_target_properties(overlay_layout_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME overlay_layout_tests COMMAND overlay_layout_tests)

# v0.6.0 codec workers: the payload arena, the ring's slot state machine, the
# degradation policy and a concurrency stress test of ring + arena + pipeline
# with a fake codec (threads, no FFmpeg).
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

add_executable(
  degradation_policy_tests
  degradation_policy_tests.cpp
  check.hpp
  "${_src}/degradation-policy.cpp"
  "${_src}/degradation-policy.hpp"
  "${_src}/buffer-fit.cpp"
  "${_src}/buffer-fit.hpp"
)
target_include_directories(degradation_policy_tests PRIVATE "${_src}")
set_target_properties(degradation_policy_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME degradation_policy_tests COMMAND degradation_policy_tests)

add_executable(
  pipeline_stress_tests
  pipeline_stress_tests.cpp
  check.hpp
  "${_src}/codec-pipeline.cpp"
  "${_src}/codec-pipeline.hpp"
  "${_src}/frame-ring.cpp"
  "${_src}/frame-ring.hpp"
  "${_src}/frame-arena.cpp"
  "${_src}/frame-arena.hpp"
  "${_src}/perf-stats.cpp"
  "${_src}/perf-stats.hpp"
)
target_include_directories(pipeline_stress_tests PRIVATE "${_src}")
target_link_libraries(pipeline_stress_tests PRIVATE Threads::Threads)
set_target_properties(pipeline_stress_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME pipeline_stress_tests COMMAND pipeline_stress_tests)

# v0.6.0 account, Trigi and ticket client.
function(trigglow_add_client_test name)
  add_executable(${name} ${ARGN} check.hpp test-support.hpp)
  target_include_directories(${name} PRIVATE "${_src}")
  set_target_properties(${name} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  if(WIN32)
    target_link_libraries(${name} PRIVATE winhttp crypt32)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

trigglow_add_client_test(json_tests json_tests.cpp "${_src}/json.cpp" "${_src}/json.hpp")
# Redaction of OBS's log before it is attached to a ticket (src/log-redact.hpp).
trigglow_add_client_test(log_redact_tests log_redact_tests.cpp "${_src}/log-redact.cpp" "${_src}/log-redact.hpp")
# Safe rendering of Trigi's replies (src/chat-text.hpp).
trigglow_add_client_test(chat_text_tests chat_text_tests.cpp "${_src}/chat-text.cpp" "${_src}/chat-text.hpp")
# The opt-in technical data of a Trigi conversation (src/support-diagnostics.hpp).
trigglow_add_client_test(
  support_diagnostics_tests
  support_diagnostics_tests.cpp
  "${_src}/support-diagnostics.cpp"
  "${_src}/support-diagnostics.hpp"
  "${_src}/pipeline-health.hpp"
  "${_src}/json.cpp"
  "${_src}/json.hpp"
)
