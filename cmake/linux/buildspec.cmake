# CMake Linux build dependencies module

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_linux: Set up Linux slice for _check_dependencies
#
# v0.7.1: libobs and obs-frontend-api of the OBS Studio pinned in
# buildspec.json (the oldest OBS supported), built from its source tarball
# into .deps like on Windows and macOS, not the distro's or the PPA's newer
# one: since OBS 32.0 a module built against a newer libobs than the running
# OBS is refused. Only built as something to compile and link against; the
# package never ships them. Qt comes from the distro (Ubuntu 24.04's Qt 6.4,
# the oldest the package declares), FFmpeg is the plugin's own static build
# (.github/scripts/linux/build-ffmpeg.sh).
function(_check_dependencies_linux)
  set(arch ${CMAKE_SYSTEM_PROCESSOR})
  set(platform linux-${arch})

  set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
  set(obs-studio_filename "VERSION.tar.gz")
  set(obs-studio_destination "obs-studio-VERSION")
  set(dependencies_list obs-studio)

  _check_dependencies()
endfunction()

_check_dependencies_linux()
