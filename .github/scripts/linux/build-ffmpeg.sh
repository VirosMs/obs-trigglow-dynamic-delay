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
# Source: FFmpeg's own git repository (its GitHub mirror) at the release tag
# pinned by buildspec.json's "ffmpeg-source" ("tag", signed by FFmpeg), built
# only if that tag is the pinned commit ("commit"). Licence: LGPL 2.1 or
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
repo_url=$(jq -r '.dependencies["ffmpeg-source"].baseUrl' "${spec}")
tag=$(jq -r '.dependencies["ffmpeg-source"].tag' "${spec}")
commit=$(jq -r '.dependencies["ffmpeg-source"].commit // empty' "${spec}")

if [[ ! ${commit} =~ ^[0-9a-f]{40}$ ]]; then
  echo "${0##*/}: buildspec.json has no ffmpeg-source commit" >&2
  exit 1
fi

prefix="${root}/.deps/ffmpeg-linux-${arch}"
work="${root}/.deps/ffmpeg-${version}-build"
stamp="${prefix}/.trigglow-build"
want="${version} ${tag} ${commit} $(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"

if [[ -f ${stamp} && $(<"${stamp}") == "${want}" && -f ${prefix}/lib/libavcodec.a ]]; then
  echo "FFmpeg ${version} (static, MJPEG only) is up to date in ${prefix}"
  exit 0
fi

rm -rf "${work}" "${prefix}"
mkdir -p "${work}"
git -C "${work}" init --quiet
git -C "${work}" fetch --quiet --depth 1 "${repo_url}.git" "refs/tags/${tag}:refs/tags/${tag}"
# Never build anything else than the pinned commit.
fetched=$(git -C "${work}" rev-parse "refs/tags/${tag}^{commit}")
if [[ ${fetched} != "${commit}" ]]; then
  echo "::error title=FFmpeg source::${tag} is ${fetched}, not the pinned ${commit}" >&2
  exit 1
fi
git -C "${work}" -c advice.detachedHead=false checkout --quiet "${commit}"

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
  echo "Source: ${repo_url}, tag ${tag}"
  echo "Commit: ${commit}"
  echo "Configure: ${configure_args[*]#--prefix=*}"
} | sed "s|${prefix}|<prefix>|g" > "${prefix}/share/licenses/ffmpeg-build.txt"
popd > /dev/null

rm -rf "${work}"
echo "${want}" > "${stamp}"
echo "FFmpeg ${version} (static, MJPEG only, LGPL 2.1 or later) installed in ${prefix}"
