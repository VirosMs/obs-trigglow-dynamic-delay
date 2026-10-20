#!/usr/bin/env bash
# Checks the staged Linux plugin (build.sh) and packages it, the same formats
# the Trigglow desktop app ships for Linux:
#
#   release/<name>-<version>-x86_64-linux-gnu.deb          Debian, Ubuntu, Mint, Pop!_OS
#   release/<name>-<version>-x86_64-linux-gnu.rpm          Fedora, openSUSE
#   release/<name>-<version>-x86_64-linux-gnu.pkg.tar.zst  Arch, Manjaro, EndeavourOS
#   release/<name>-<version>-x86_64-linux-gnu.tar.xz       any distro, per-user install
#   release/<name>-<version>-x86_64-linux-gnu-dbgsym.tar.xz  debug symbols (CI artifact only)
#   release/<name>-<version>-ffmpeg-<tag>-source.tar.xz  source of the FFmpeg linked in (LGPL)
#
# The website looks for the "-x86_64-linux-gnu.deb" suffix: keep the names.
#
# The system packages install one .so and one data folder in OBS 33's
# locations and relative symlinks in the legacy ones OBS 31 and 32 read:
#   <libdir>/obs-modules/plugins/<name>.so            (OBS 33 and later)
#   <libdir>/obs-plugins/<name>.so  -> the above      (OBS 31, 32)
#   /usr/share/obs/obs-modules/plugins/<name>/        (OBS 33 and later)
#   /usr/share/obs/obs-plugins/<name>  -> the above   (OBS 31, 32)
# <libdir> is /usr/lib/x86_64-linux-gnu (.deb), /usr/lib64 (.rpm) or /usr/lib
# (.pkg.tar.zst): wherever that family's OBS looks.
#
# Dependencies are declared by hand, not by dpkg-shlibdeps (which pinned the
# build machine's exact libavcodec and Qt package names): libc and libstdc++
# from the newest symbol versions the .so actually uses, Qt 6.4 or newer
# (each family's names; the Debian ones with and without the t64 suffix), and
# OBS Studio at buildspec.json's obsCompat minimum. FFmpeg is linked in.
#
# The packager, nfpm, is always the release pinned in buildspec.json,
# downloaded and checked against its SHA-256 (never one already on PATH).
#
# Usage: package.sh

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
spec="${root}/buildspec.json"
arch=$(uname -m)
name=$(jq -r '.name' "${spec}")
version=$(jq -r '.version' "${spec}")
obs_min=$(jq -r '.obsCompat.minimum' "${spec}")
website=$(jq -r '.website' "${spec}")
email=$(jq -r '.email' "${spec}")
debian_libdir="lib/${arch}-linux-gnu"
stage="${root}/release/stage"
so="${stage}/usr/${debian_libdir}/obs-modules/plugins/${name}.so"
data="${stage}/usr/share/obs/obs-modules/plugins/${name}"
ffmpeg="${root}/.deps/ffmpeg-linux-${arch}"
output_base="${name}-${version}-${arch}-linux-gnu"
out="${root}/release"

fail() {
  echo "::error::${*}" >&2
  exit 1
}

[[ ${arch} == x86_64 ]] || fail "only x86_64 is packaged (this is ${arch})"
[[ -f ${so} && ! -L ${so} ]] || fail "no staged plugin at ${so}: run build.sh first"
[[ -f ${data}/locale/en-US.ini ]] || fail "no staged data folder at ${data}"
[[ -L ${stage}/usr/${debian_libdir}/obs-plugins/${name}.so ]] || fail 'legacy .so symlink missing'
[[ -L ${stage}/usr/share/obs/obs-plugins/${name} ]] || fail 'legacy data symlink missing'

echo "::group::Check the plugin binary"
readelf -d "${so}" | grep -E 'NEEDED|SONAME|RPATH|RUNPATH' || true
needed=$(readelf -d "${so}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
if grep -E '^lib(av|sw|postproc)' <<< "${needed}"; then
  fail 'the plugin links a shared FFmpeg: it must use the static one (CMakeLists.txt, OS_LINUX)'
fi
if readelf -d "${so}" | grep -qE 'R(UN)?PATH'; then
  fail 'the plugin has an RPATH/RUNPATH (the build tree must not leak into the package)'
fi
exported=$(nm -D --defined-only "${so}" | awk '{ print $NF }' | sort -u)
echo "Exported: $(tr '\n' ' ' <<< "${exported}")"
if grep -E '^(av|ff_|avpriv_|avcodec|swr_|sws_)' <<< "${exported}"; then
  fail 'the plugin exports FFmpeg symbols: they would clash with the avcodec OBS loads'
fi
others=$(grep -vE '^(obs_module_.*|obs_current_module|_init|_fini)$' <<< "${exported}" || true)
[[ -z ${others} ]] || fail "unexpected exported symbols (cmake/linux/plugin-exports.map): ${others}"

glibc=$(objdump -T "${so}" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sed 's/GLIBC_//' | sort -Vu | tail -n 1)
glibcxx=$(objdump -T "${so}" | grep -oE 'GLIBCXX_[0-9]+(\.[0-9]+)+' | sed 's/GLIBCXX_//' | sort -Vu | tail -n 1)
echo "Newest symbol versions used: GLIBC_${glibc}, GLIBCXX_${glibcxx}"
# Ubuntu 24.04 (glibc 2.39, libstdc++ of GCC 14) is the oldest supported.
[[ $(printf '%s\n' "${glibc}" 2.39 | sort -V | tail -n 1) == 2.39 ]] \
  || fail "needs glibc ${glibc}, newer than Ubuntu 24.04's 2.39: build on Ubuntu 24.04"
case ${glibcxx} in
  3.4.29) gcc_min=11 ;;
  3.4.30) gcc_min=12 ;;
  3.4.31 | 3.4.32) gcc_min=13 ;;
  3.4.33) gcc_min=14 ;;
  *) fail "needs GLIBCXX_${glibcxx}: not mapped to a GCC release here (or newer than Ubuntu 24.04's)" ;;
esac
echo "::endgroup::"

echo "::group::Split the debug symbols"
dbg_dir="${root}/release/dbgsym/${output_base}-dbgsym"
rm -rf "${root}/release/dbgsym"
mkdir -p "${dbg_dir}"
objcopy --only-keep-debug "${so}" "${dbg_dir}/${name}.so.debug"
strip --strip-unneeded "${so}"
objcopy --add-gnu-debuglink="${dbg_dir}/${name}.so.debug" "${so}"
tar -C "${root}/release/dbgsym" -cJf "${out}/${output_base}-dbgsym.tar.xz" "${output_base}-dbgsym"
ls -l "${so}"
echo "::endgroup::"

echo "::group::FFmpeg licence"
# data/licenses/LICENSE-ffmpeg.txt describes both builds; the Linux packages
# also carry the LGPL 2.1 text and the exact build of their static FFmpeg.
[[ -f ${ffmpeg}/share/licenses/COPYING.LGPLv2.1 ]] || fail 'no FFmpeg licence in .deps (build-ffmpeg.sh)'
cp "${ffmpeg}/share/licenses/COPYING.LGPLv2.1" "${data}/licenses/LICENSE-ffmpeg-LGPL-2.1.txt"
cp "${ffmpeg}/share/licenses/ffmpeg-build.txt" "${data}/licenses/ffmpeg-build-linux.txt"
cat "${data}/licenses/ffmpeg-build-linux.txt"
# The LGPL also asks for the source of that FFmpeg: the exact tree it was
# built from (git archive of the pinned commit, build-ffmpeg.sh), published
# with the release next to the packages.
ffmpeg_tag=$(jq -r '.dependencies["ffmpeg-source"].tag' "${spec}")
ffmpeg_source="${ffmpeg}/share/source/ffmpeg-${ffmpeg_tag}.tar.xz"
[[ -s ${ffmpeg_source} ]] || fail "no FFmpeg source tarball at ${ffmpeg_source} (build-ffmpeg.sh)"
cp "${ffmpeg_source}" "${out}/${name}-${version}-ffmpeg-${ffmpeg_tag}-source.tar.xz"
echo "${name}-${version}-ffmpeg-${ffmpeg_tag}-source.tar.xz: $(sha256sum "${ffmpeg_source}" | cut -d ' ' -f 1)"
echo "::endgroup::"

echo "::group::nfpm"
# Always the release pinned in buildspec.json, checked against its SHA-256,
# never an nfpm found on PATH: what builds the packages is part of the build.
nfpm_version=$(jq -r '.dependencies.nfpm.version' "${spec}")
nfpm_url=$(jq -r '.dependencies.nfpm.baseUrl' "${spec}")
nfpm_hash=$(jq -r --arg platform "linux-${arch}" '.dependencies.nfpm.hashes[$platform] // empty' "${spec}")
[[ ${nfpm_hash} =~ ^[0-9a-f]{64}$ ]] || fail "buildspec.json has no nfpm hash for linux-${arch}"
nfpm_dir="${root}/.deps/nfpm-${nfpm_version}"
nfpm_tar="${nfpm_dir}/nfpm_${nfpm_version}_Linux_${arch}.tar.gz"
mkdir -p "${nfpm_dir}"
if [[ ! -f ${nfpm_tar} ]] || ! echo "${nfpm_hash}  ${nfpm_tar}" | sha256sum --check --status; then
  rm -f "${nfpm_tar}"
  curl --fail --location --proto '=https' --tlsv1.2 --retry 3 --silent --show-error \
    --output "${nfpm_tar}" "${nfpm_url}/v${nfpm_version}/nfpm_${nfpm_version}_Linux_${arch}.tar.gz"
fi
echo "${nfpm_hash}  ${nfpm_tar}" | sha256sum --check -
# Extracted afresh from the checked archive every time.
rm -f "${nfpm_dir}/nfpm"
tar -xzf "${nfpm_tar}" -C "${nfpm_dir}" nfpm
nfpm_bin="${nfpm_dir}/nfpm"
"${nfpm_bin}" --version
echo "::endgroup::"

# One "  - a" YAML list line per argument.
yaml_list() {
  local item
  for item in "$@"; do
    printf '      - "%s"\n' "${item}"
  done
}

# Debian and Ubuntu: Qt's t64 names (24.04, and Core everywhere) or the plain
# ones (Gui, Widgets and Svg since 25.04 and on Debian 13).
deb_depends=(
  "libc6 (>= ${glibc})"
  "libstdc++6 (>= ${gcc_min})"
  "libgcc-s1"
  "libqt6core6t64 (>= 6.4) | libqt6core6 (>= 6.4)"
  "libqt6gui6t64 (>= 6.4) | libqt6gui6 (>= 6.4)"
  "libqt6widgets6t64 (>= 6.4) | libqt6widgets6 (>= 6.4)"
  "libqt6svg6t64 (>= 6.4) | libqt6svg6 (>= 6.4)"
  "obs-studio (>= ${obs_min})"
)
# Fedora and openSUSE name their packages differently, so the libraries are
# required by soname, as rpmbuild itself would (Qt 6.4 or newer is all they
# ship), plus OBS Studio by name for the minimum version.
rpm_depends=("obs-studio >= ${obs_min}")
while read -r lib; do
  [[ -n ${lib} ]] && rpm_depends+=("${lib}()(64bit)")
done <<< "${needed}"
rpm_depends+=("libc.so.6(GLIBC_${glibc})(64bit)" "libstdc++.so.6(GLIBCXX_${glibcxx})(64bit)")
arch_depends=(
  "glibc>=${glibc}"
  "libstdc++>=${gcc_min}"
  "libgcc"
  "qt6-base>=6.4"
  "qt6-svg>=6.4"
  "obs-studio>=${obs_min}"
)

contents_for() {
  local packager=$1 libdir=$2
  cat << EOF
  - src: ${so}
    dst: ${libdir}/obs-modules/plugins/${name}.so
    packager: ${packager}
    file_info:
      mode: 0644
  - src: ../obs-modules/plugins/${name}.so
    dst: ${libdir}/obs-plugins/${name}.so
    type: symlink
    packager: ${packager}
EOF
}

nfpm_config="${root}/release/nfpm.yaml"
cat > "${nfpm_config}" << EOF
name: ${name}
arch: amd64
platform: linux
version: ${version}
release: 1
section: video
priority: optional
maintainer: Trigglow <${email}>
vendor: Trigglow
homepage: ${website}
license: GPL-2.0-or-later
description: |-
  Trigglow Dynamic Delay for OBS Studio
  Delays your stream's video and audio by a configurable number of seconds
  without reconnecting the stream. Needs OBS Studio ${obs_min} or newer.
contents:
$(contents_for deb "/usr/${debian_libdir}")
$(contents_for rpm /usr/lib64)
$(contents_for archlinux /usr/lib)
  - src: ${data}
    dst: /usr/share/obs/obs-modules/plugins/${name}
    type: tree
  - src: ../obs-modules/plugins/${name}
    dst: /usr/share/obs/obs-plugins/${name}
    type: symlink
overrides:
  deb:
    depends:
$(yaml_list "${deb_depends[@]}")
  rpm:
    depends:
$(yaml_list "${rpm_depends[@]}")
  archlinux:
    depends:
$(yaml_list "${arch_depends[@]}")
rpm:
  group: Applications/Multimedia
  summary: Trigglow Dynamic Delay for OBS Studio
archlinux:
  packager: Trigglow <${email}>
EOF

echo "::group::nfpm.yaml"
cat "${nfpm_config}"
echo "::endgroup::"

rm -f "${out}/${output_base}".{deb,rpm,pkg.tar.zst,tar.xz}
"${nfpm_bin}" package --config "${nfpm_config}" --packager deb --target "${out}/${output_base}.deb"
"${nfpm_bin}" package --config "${nfpm_config}" --packager rpm --target "${out}/${output_base}.rpm"
"${nfpm_bin}" package --config "${nfpm_config}" --packager archlinux --target "${out}/${output_base}.pkg.tar.zst"

echo "::group::Per-user archive"
user_dir="${root}/release/user/${output_base}"
rm -rf "${root}/release/user"
mkdir -p "${user_dir}/${name}"
cp "${so}" "${user_dir}/${name}/${name}.so"
cp -R "${data}" "${user_dir}/${name}/data"
install -m 0755 "${root}/installers/linux/install.sh" "${user_dir}/install.sh"
sed "s/@OBS_MIN_VERSION@/${obs_min}/g" "${root}/installers/linux/README.txt" > "${user_dir}/README.txt"
chmod 0644 "${user_dir}/README.txt"
tar -C "${root}/release/user" -cJf "${out}/${output_base}.tar.xz" "${output_base}"
tar -tJvf "${out}/${output_base}.tar.xz"
echo "::endgroup::"

echo "::group::Packages"
for package in "${out}/${output_base}".{deb,rpm,pkg.tar.zst,tar.xz}; do
  [[ -s ${package} ]] || fail "missing ${package}"
  echo "${package##*/}: $(sha256sum "${package}" | cut -d ' ' -f 1)"
done
dpkg-deb --info "${out}/${output_base}.deb"
dpkg-deb --contents "${out}/${output_base}.deb"
echo "::endgroup::"
