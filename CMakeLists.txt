cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# FFmpeg (avcodec/avutil) -- v0.3.0 real-compression groundwork (see
# docs/ROADMAP.md, docs/SPEC.md). Phase 0 only: link + a version-string smoke
# test (src/plugin-main.cpp), no codec logic uses it yet. Windows + Linux
# only this pass -- macOS has no equivalent trusted prebuilt to BtbN's
# Windows builds and isn't the priority platform yet (see docs/ROADMAP.md);
# TRIGGLOW_HAVE_FFMPEG gates the smoke-test code so macOS keeps compiling
# with zero FFmpeg dependency.
if(OS_WINDOWS)
  # BtbN's package ships only pre-versioned DLL import libs (avcodec.lib +
  # matching avcodec-63.dll etc.), not a true static archive -- so this is
  # dynamic linking, not the fully-static approach originally favored to
  # sidecar-side-step any DLL-name collision with OBS's own bundled
  # obs-ffmpeg plugin (which loads its own avcodec-*.dll into the same
  # process). Mitigation: install() below copies our exact copies of the
  # needed DLLs into bin/64bit right next to this plugin's own binary --
  # Windows' DLL search order checks a loading module's own directory before
  # falling back to the process-wide search path, so as long as obs-ffmpeg's
  # copy lives elsewhere (its own plugin folder), this plugin's imports
  # resolve to OUR copies. Not airtight (a same-named-and-located DLL from
  # another source could still collide) but the realistic risk given what's
  # actually available from BtbN today.
  file(GLOB _ffmpeg_root_candidates "${CMAKE_CURRENT_SOURCE_DIR}/.deps/ffmpeg-windows-x64/*")
  list(LENGTH _ffmpeg_root_candidates _ffmpeg_root_count)
  if(NOT _ffmpeg_root_count EQUAL 1)
    message(
      FATAL_ERROR
      "Expected exactly one extracted directory under .deps/ffmpeg-windows-x64, found ${_ffmpeg_root_count} -- "
      "delete .deps/ffmpeg-windows-x64 and reconfigure to re-extract."
    )
  endif()
  list(GET _ffmpeg_root_candidates 0 _ffmpeg_root)

  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${_ffmpeg_root}/include")
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "${_ffmpeg_root}/lib/avcodec.lib" "${_ffmpeg_root}/lib/avutil.lib"
  )
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE TRIGGLOW_HAVE_FFMPEG)

  # Same DESTINATION set_target_properties_plugin() (cmake/windows/helpers.cmake)
  # uses for the plugin binary itself -- these need to land right next to it,
  # not just anywhere under the packaged output. Deliberately not also
  # copying into rundir/$<CONFIG> (the local-dev convenience copy
  # set_target_properties_plugin() maintains for the main target): its
  # POST_BUILD make_directory call is registered later (that function runs
  # after this block), so an earlier POST_BUILD copy here would race it and
  # fail against a directory that doesn't exist yet. install() has no such
  # ordering pitfall -- every install() rule runs together at `cmake
  # --install` time, which is what CI's actual packaging step uses.
  # swresample-7.dll: avcodec-63.dll has a real (non-delay-loaded) PE import
  # dependency on it -- verified 2026-08-27 with pefile against the exact
  # pinned BtbN package after Phase 1's real avcodec_* calls (not just
  # avutil's av_version_info() smoke test) failed to load in a real OBS
  # install with "LoadLibrary failed ... (126)". avcodec.lib/.dll alone was
  # never actually exercised at runtime until Phase 1, so this was missing
  # from day one without ever being caught. swresample-7.dll's own only
  # non-system dependency is avutil-61.dll (already shipped below).
  install(
    FILES
      "${_ffmpeg_root}/bin/avcodec-63.dll"
      "${_ffmpeg_root}/bin/avutil-61.dll"
      "${_ffmpeg_root}/bin/swresample-7.dll"
    DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit"
  )
elseif(OS_LINUX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::FFMPEG)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE TRIGGLOW_HAVE_FFMPEG)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)

  if(OS_WINDOWS)
    # AuthManager's HTTPS calls (see docs/ACCOUNT_GATE.md, src/auth-manager.hpp,
    # src/win-http.hpp) go through WinHTTP, not Qt's QNetworkAccessManager --
    # Qt's TLS backend plugin is tied to the EXACT Qt minor.patch version of
    # whatever Qt6Network.dll happens to already be loaded in the host
    # process, which for a plugin running inside OBS is OBS's OWN bundled
    # Qt, not the (older, obs-deps-vendored) Qt this plugin builds against --
    # confirmed live, 2026-09-01: OBS 32.2.2 ships Qt 6.11.1, obs-deps'
    # 2025-07-11 package is Qt 6.8.3, and shipping the 6.8.3 TLS plugin
    # anyway just gets silently rejected ("No functional TLS backend was
    # found") rather than working. WinHTTP has no such dependency: TLS is
    # handled by the OS itself.
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp)
  endif()

  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/delay-controller.cpp
    src/delay-controller.hpp
    src/obs-frontend-bridge.cpp
    src/obs-frontend-bridge.hpp
    src/settings-ui.cpp
    src/settings-ui.hpp
    src/hotkeys.cpp
    src/hotkeys.hpp
    src/logging.cpp
    src/logging.hpp
    src/video-delay-filter.cpp
    src/video-delay-filter.hpp
    src/audio-delay-filter.cpp
    src/audio-delay-filter.hpp
    src/hardware-info.cpp
    src/hardware-info.hpp
    src/scene-combo-box.cpp
    src/scene-combo-box.hpp
    src/buffer-mode-controller.cpp
    src/buffer-mode-controller.hpp
    src/auth-manager.cpp
    src/auth-manager.hpp
    src/win-http.cpp
    src/win-http.hpp
)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
