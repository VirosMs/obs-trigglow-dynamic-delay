#!/usr/bin/env bash
# Build dependencies of the Linux plugin, for an ubuntu:24.04 container run
# as root (build-project.yaml's linux-build job).
#
# Ubuntu 24.04 is the build baseline on purpose: the plugin then needs glibc
# 2.39 and Qt 6.4 at most, which Ubuntu 24.04, 26.04, 26.10, Debian 13,
# Fedora and Arch all have. Everything is from Ubuntu's own archive, no PPA:
# libobs is built from the pinned OBS sources (cmake/linux/buildspec.cmake)
# and FFmpeg from the pinned FFmpeg sources (build-ffmpeg.sh). The distro's
# FFmpeg -dev packages below are only for building that libobs (it needs
# avformat, swscale and swresample); the plugin never links them.

set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "${0##*/}: run as root (it calls apt-get)" >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

packages=(
  # Toolchain and tools used by the build and packaging scripts (gpg and
  # gpg-agent: the FFmpeg tag's signature, build-ffmpeg.sh; gpg --import
  # starts the agent and fails without it in a minimal container)
  build-essential
  ca-certificates
  ccache
  cmake
  curl
  file
  git
  gpg
  gpg-agent
  jq
  nasm
  ninja-build
  pkg-config
  xz-utils
  zstd
  # libobs 31.1.1 (libobs/CMakeLists.txt, libobs/cmake/os-linux.cmake,
  # libobs-opengl) with UI, scripting, PulseAudio and Wayland off. OBS's own
  # cmake/linux/ecmconfig.cmake requires ECM even for libobs alone.
  extra-cmake-modules
  libavcodec-dev
  libavformat-dev
  libavutil-dev
  libswresample-dev
  libswscale-dev
  libdrm-dev
  libegl-dev
  libgl-dev
  libgles2-mesa-dev
  libglib2.0-dev
  libjansson-dev
  libsimde-dev
  libx11-dev
  libx11-xcb-dev
  libxcb1-dev
  libxcb-xinput-dev
  uthash-dev
  uuid-dev
  zlib1g-dev
  # The plugin's Qt (Ubuntu 24.04: Qt 6.4.2, the oldest the packages accept)
  qt6-base-dev
  qt6-base-private-dev
  libqt6svg6-dev
)

apt-get update -q
apt-get install -y -q --no-install-recommends "${packages[@]}"
