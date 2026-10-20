#!/usr/bin/env bash
# Configures, builds and stages the Linux plugin (build-project.yaml's
# linux-build job, inside ubuntu:24.04 after install-build-deps.sh and
# build-ffmpeg.sh).
#
# - libobs and obs-frontend-api: the OBS Studio pinned in buildspec.json,
#   built from source into .deps on the first configure
#   (cmake/linux/buildspec.cmake), never the distro's or the PPA's.
# - Staged with DESTDIR into release/stage/usr: OBS 33's
#   lib/x86_64-linux-gnu/obs-modules/plugins/ and share/obs/obs-modules/plugins/,
#   plus the legacy obs-plugins symlinks for OBS 31 and 32
#   (cmake/linux/helpers.cmake). package.sh turns that into the packages.
#
# Usage: build.sh [Debug|RelWithDebInfo|Release|MinSizeRel]

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
config=${1:-RelWithDebInfo}
preset=ubuntu-x86_64
[[ -n ${CI:-} ]] && preset=ubuntu-ci-x86_64
build_dir="${root}/build_x86_64"

cd "${root}"

echo "::group::Configure"
cmake --preset "${preset}" -DCMAKE_BUILD_TYPE="${config}" -DCMAKE_INSTALL_PREFIX=/usr
echo "::endgroup::"

echo "::group::Build"
cmake --build --preset "${preset}" --config "${config}" --parallel
echo "::endgroup::"

echo "::group::Stage"
rm -rf "${root}/release/stage"
DESTDIR="${root}/release/stage" cmake --install "${build_dir}" --config "${config}" --prefix /usr
echo "::endgroup::"

echo "::group::Source tarball"
cmake --build "${build_dir}" --config "${config}" --target package_source
echo "::endgroup::"
