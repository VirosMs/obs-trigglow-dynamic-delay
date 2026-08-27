# CMake Windows build dependencies module

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_windows: Set up Windows slice for _check_dependencies
function(_check_dependencies_windows)
  set(arch ${CMAKE_VS_PLATFORM_NAME})
  set(platform windows-${arch})

  set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
  set(prebuilt_filename "windows-deps-VERSION-ARCH-REVISION.zip")
  set(prebuilt_destination "obs-deps-VERSION-ARCH")
  set(qt6_filename "windows-deps-qt6-VERSION-ARCH-REVISION.zip")
  set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
  set(obs-studio_filename "VERSION.zip")
  set(obs-studio_destination "obs-studio-VERSION")
  # BtbN's own release asset filenames bake in a git-describe hash we can't
  # predict/template (e.g. "ffmpeg-n9.0.1-8-g16dfae5c88-win64-lgpl-shared-9.0.zip"),
  # unlike obs-deps' VERSION/ARCH/REVISION-templated names above -- so this is a
  # literal pin, not a template. Re-pinning to a newer BtbN build means updating
  # this filename + buildspec.json's ffmpeg.version/hashes.windows-x64 together;
  # the fixed "ffmpeg-windows-x64" destination below means CMakeLists.txt's
  # extraction-root glob never needs touching on a re-pin.
  set(ffmpeg_filename "ffmpeg-n9.0.1-8-g16dfae5c88-win64-lgpl-shared-9.0.zip")
  set(ffmpeg_destination "ffmpeg-windows-x64")
  set(dependencies_list prebuilt qt6 obs-studio ffmpeg)

  _check_dependencies()
endfunction()

_check_dependencies_windows()
