#!/usr/bin/env bash
# The plugin's own FFmpeg on Linux: libavcodec and libavutil, static, with
# only the MJPEG encoder and decoder (src/mjpeg-codec.cpp), installed into
# .deps/ffmpeg-linux-<arch> for CMakeLists.txt's OS_LINUX block.
#
# Why our own and static: the distro's FFmpeg has a different soname on each
# release (libavcodec60 on Ubuntu 24.04, 61 on Debian 13, 62 on Ubuntu 26.04),
# so a package linked to one installs nowhere else. Compiled with
# -fvisibility=hidden and linked with --exclude-libs and a version script,
# none of its symbols is exported, so it never clashes with the avcodec OBS
# itself loads into the same process.
#
# Source: the release tarball pinned by buildspec.json's "ffmpeg-source"
# (version and SHA-256), checked before it is unpacked. Licence: LGPL 2.1 or
# later. The build stops if FFmpeg's configure reports anything else (a GPL,
# version 3 or nonfree build), the same guard the Windows job applies to its
# FFmpeg DLLs.
#
# Usage: build-ffmpeg.sh   (from anywhere; rebuilds only when the pin or
# this script change)

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
arch=$(uname -m)
spec="${root}/buildspec.json"

version=$(jq -r '.dependencies["ffmpeg-source"].version' "${spec}")
base_url=$(jq -r '.dependencies["ffmpeg-source"].baseUrl' "${spec}")
hash=$(jq -r --arg platform "linux-${arch}" '.dependencies["ffmpeg-source"].hashes[$platform] // empty' "${spec}")

if [[ ! ${hash} =~ ^[0-9a-f]{64}$ ]]; then
  echo "${0##*/}: buildspec.json has no ffmpeg-source hash for linux-${arch}" >&2
  exit 1
fi

prefix="${root}/.deps/ffmpeg-linux-${arch}"
tarball="${root}/.deps/ffmpeg-${version}.tar.xz"
work="${root}/.deps/ffmpeg-${version}-build"
stamp="${prefix}/.trigglow-build"
want="${version} ${hash} $(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"

if [[ -f ${stamp} && $(<"${stamp}") == "${want}" && -f ${prefix}/lib/libavcodec.a ]]; then
  echo "FFmpeg ${version} (static, MJPEG only) is up to date in ${prefix}"
  exit 0
fi

mkdir -p "${root}/.deps"
if [[ ! -f ${tarball} ]] || ! echo "${hash}  ${tarball}" | sha256sum --check --status; then
  rm -f "${tarball}"
  curl --fail --location --proto '=https' --tlsv1.2 --retry 3 --silent --show-error \
    --output "${tarball}.part" "${base_url}/ffmpeg-${version}.tar.xz"
  mv "${tarball}.part" "${tarball}"
fi
# Never unpack anything else than the pinned tarball.
echo "${hash}  ${tarball}" | sha256sum --check -

rm -rf "${work}" "${prefix}"
mkdir -p "${work}"
tar -xJf "${tarball}" -C "${work}" --strip-components=1

configure_args=(
  --prefix="${prefix}"
  --enable-static
  --disable-shared
  --enable-pic
  --disable-debug
  --disable-doc
  --disable-programs
  --disable-network
  --disable-autodetect
  --disable-everything
  --disable-avdevice
  --disable-avformat
  --disable-avfilter
  --disable-swscale
  --disable-swresample
  --enable-avcodec
  --enable-avutil
  --enable-encoder=mjpeg
  --enable-decoder=mjpeg
  --extra-cflags=-fvisibility=hidden
)

pushd "${work}" > /dev/null
./configure "${configure_args[@]}"

# Licence guard: only an LGPL 2.1-or-later build may be linked into the
# plugin (data/licenses/LICENSE-ffmpeg.txt says so).
fail_license() {
  echo "::error title=FFmpeg licence::${1}" >&2
  exit 1
}
grep -qx '#define CONFIG_GPL 0' config.h || fail_license 'configured with --enable-gpl'
grep -qx '#define CONFIG_NONFREE 0' config.h || fail_license 'configured with --enable-nonfree'
grep -qx '#define CONFIG_VERSION3 0' config.h || fail_license 'configured with --enable-version3'
grep -qx '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' config.h \
  || fail_license "licence is $(grep FFMPEG_LICENSE config.h || echo unknown), not LGPL version 2.1 or later"
components=config_components.h
[[ -f ${components} ]] || components=config.h
for component in CONFIG_MJPEG_ENCODER CONFIG_MJPEG_DECODER; do
  grep -qx "#define ${component} 1" "${components}" || {
    echo "${0##*/}: ${component} is not enabled" >&2
    exit 1
  }
done

make -j"$(nproc)"
make install

# The licence travels with the Linux packages (.github/scripts/linux/package.sh).
mkdir -p "${prefix}/share/licenses"
cp COPYING.LGPLv2.1 "${prefix}/share/licenses/COPYING.LGPLv2.1"
{
  echo "FFmpeg ${version}"
  echo "Source: ${base_url}/ffmpeg-${version}.tar.xz"
  echo "SHA-256: ${hash}"
  echo "Configure: ${configure_args[*]#--prefix=*}"
} | sed "s|${prefix}|<prefix>|g" > "${prefix}/share/licenses/ffmpeg-build.txt"
popd > /dev/null

rm -rf "${work}"
echo "${want}" > "${stamp}"
echo "FFmpeg ${version} (static, MJPEG only, LGPL 2.1 or later) installed in ${prefix}"
