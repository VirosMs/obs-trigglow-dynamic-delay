#!/usr/bin/env bash
# Installs a Linux package the way a user would and checks the plugin it
# installs (build-project.yaml's linux-verify job, one clean distro container
# per run, as root):
#
#   deb     apt-get install ./<name>-<version>-x86_64-linux-gnu.deb, resolving
#           its dependencies from the distro (and from the OBS PPA on an
#           Ubuntu whose own OBS is older than buildspec.json's minimum)
#   rpm     dnf install ./<name>-...-x86_64-linux-gnu.rpm
#   pacman  pacman -U ./<name>-...-x86_64-linux-gnu.pkg.tar.zst
#   user    the .tar.xz's install.sh as an ordinary user, then --uninstall
#
# Then, for the installed .so: every library found (ldd), every symbol
# resolved (ldd -r), no FFmpeg symbol exported (nm -D), no shared FFmpeg
# linked, and the legacy OBS 31/32 paths linking to the OBS 33 ones.
#
# A distro whose own OBS is below the minimum and has no PPA (Debian 13 ships
# OBS 30.2) must refuse the .deb: that refusal is checked, then the package is
# forced in next to the distro's OBS only to check its libraries, and an
# unresolved symbol there (an OBS 31 function libobs 30 lacks) is a warning.
#
# Usage: verify-package.sh deb|rpm|pacman|user <directory with the packages>

set -euo pipefail

family=${1:?family: deb, rpm, pacman or user}
packages_dir=$(cd "${2:?directory with the packages}" && pwd)
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
spec="${root}/buildspec.json"

fail() {
  echo "::error::${*}" >&2
  exit 1
}
warn() { echo "::warning::${*}"; }

# Without jq (not in every image): the values are simple strings.
json_value() { sed -n "s/^ \{${2}\}\"${1}\": \"\(.*\)\",\{0,1\}$/\1/p" "${spec}" | head -n 1; }
name=$(json_value name 4)
obs_min=$(json_value minimum 8)
[[ -n ${name} && -n ${obs_min} ]] || fail 'could not read name and obsCompat.minimum from buildspec.json'

package_file() {
  local file
  file=$(find "${packages_dir}" -maxdepth 2 -name "${name}-*-x86_64-linux-gnu${1}" ! -name '*-dbgsym*' | head -n 1)
  [[ -n ${file} ]] || fail "no ${name}-*-x86_64-linux-gnu${1} in ${packages_dir}"
  echo "${file}"
}

strict=1
. /etc/os-release
echo "Distro: ${PRETTY_NAME}; OBS minimum ${obs_min}"

case ${family} in
  deb | user)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q --no-install-recommends binutils ca-certificates xz-utils
    deb=$(package_file .deb)
    candidate() { apt-cache policy obs-studio | awk '/Candidate:/ { print $2 }'; }
    obs_candidate=$(candidate)
    if [[ ${ID} == ubuntu ]] && ! dpkg --compare-versions "${obs_candidate:-0}" ge "${obs_min}"; then
      echo "${PRETTY_NAME}'s obs-studio is ${obs_candidate:-missing}: adding the OBS PPA"
      apt-get install -y -q --no-install-recommends software-properties-common gpg-agent
      add-apt-repository -y ppa:obsproject/obs-studio
      apt-get update -q
      obs_candidate=$(candidate)
    fi
    if [[ ${family} == user ]]; then
      # Only what the plugin needs (OBS, Qt), not the system package itself.
      apt-get satisfy -y -q --no-install-recommends "$(dpkg-deb --field "${deb}" Depends)"
      apt-get install -y -q --no-install-recommends passwd
    elif dpkg --compare-versions "${obs_candidate:-0}" ge "${obs_min}"; then
      apt-get install -y -q --no-install-recommends "${deb}"
    else
      warn "${PRETTY_NAME} offers obs-studio ${obs_candidate:-(none)}, below ${obs_min}: the .deb must refuse to install there"
      if apt-get install -y -q --no-install-recommends --simulate "${deb}"; then
        fail "apt would install the .deb with obs-studio ${obs_candidate:-(none)}, below ${obs_min}"
      fi
      # Everything else it depends on, plus the distro's OBS for libobs.so.30.
      depends=$(dpkg-deb --field "${deb}" Depends | tr ',' '\n' | grep -v 'obs-studio' | paste -sd ',' -)
      apt-get satisfy -y -q --no-install-recommends "${depends}, obs-studio"
      dpkg --install --force-depends "${deb}"
      strict=0
    fi
    libdir="/usr/lib/$(uname -m)-linux-gnu"
    ;;
  rpm)
    dnf install -y -q binutils findutils
    dnf install -y "$(package_file .rpm)"
    libdir=/usr/lib64
    ;;
  pacman)
    pacman -Syu --noconfirm --needed binutils
    pacman -U --noconfirm "$(package_file .pkg.tar.zst)"
    libdir=/usr/lib
    ;;
  *) fail "unknown family ${family}" ;;
esac

check_plugin() {
  local so=$1 output
  [[ -f ${so} ]] || fail "plugin not installed at ${so}"
  echo "== ${so}"
  ldd "${so}"
  output=$(ldd "${so}" | grep 'not found' || true)
  if [[ -n ${output} ]]; then
    # Below the minimum OBS, only libobs itself may be missing or different.
    if ((strict)) || grep -vq 'libobs' <<< "${output}"; then
      fail "${so}: libraries not found on ${PRETTY_NAME}: ${output}"
    fi
    warn "not found with this distro's older OBS (expected below ${obs_min}): ${output}"
  fi
  if ldd "${so}" | grep -qE 'lib(avcodec|avutil|swresample|swscale)\.so'; then
    fail "${so} loads a shared FFmpeg: it must be linked statically"
  fi
  output=$(ldd -r "${so}" 2>&1 | grep 'undefined symbol' || true)
  if [[ -n ${output} ]]; then
    if ((strict)); then
      fail "${so}: unresolved symbols on ${PRETTY_NAME}: ${output}"
    fi
    warn "unresolved symbols against this distro's older OBS (expected below ${obs_min}): ${output}"
  fi
  output=$(nm -D --defined-only "${so}" | awk '{ print $NF }' | grep -E '^(av|ff_|avpriv_|swr_|sws_)' || true)
  [[ -z ${output} ]] || fail "${so} exports FFmpeg symbols: ${output}"
  echo "Exported: $(nm -D --defined-only "${so}" | awk '{ print $NF }' | tr '\n' ' ')"
}

# A legacy path must be a symlink to the OBS 33 one.
check_link() {
  local link=$1 expected=$2
  [[ -L ${link} ]] || fail "${link} is not a symlink"
  [[ $(readlink -f "${link}") == $(readlink -f "${expected}") ]] \
    || fail "${link} points to $(readlink -f "${link}"), not ${expected}"
  echo "${link} -> $(readlink "${link}")"
}

if [[ ${family} == user ]]; then
  tarball=$(package_file .tar.xz)
  useradd --create-home trigglow-test
  home=/home/trigglow-test
  # Settings that must survive an install and an uninstall.
  install -d -o trigglow-test "${home}/.config/obs-studio/plugin_config/${name}"
  echo '{}' > "${home}/.config/obs-studio/plugin_config/${name}/keep.json"
  chown -R trigglow-test "${home}/.config"
  work=$(mktemp -d)
  chmod 0755 "${work}"
  tar -xJf "${tarball}" -C "${work}"
  chown -R trigglow-test "${work}"
  if "${work}"/*/install.sh 2> /dev/null; then
    fail 'install.sh ran as root: it must refuse'
  fi
  su trigglow-test -c "${work}/*/install.sh"
  so="${home}/.local/share/obs-studio/plugins/${name}/${name}.so"
  check_plugin "${so}"
  [[ -f ${home}/.local/share/obs-studio/plugins/${name}/data/locale/en-US.ini ]] || fail 'user data folder missing'
  check_link "${home}/.config/obs-studio/plugins/${name}/bin/64bit/${name}.so" "${so}"
  check_link "${home}/.config/obs-studio/plugins/${name}/data" "${home}/.local/share/obs-studio/plugins/${name}/data"
  su trigglow-test -c "${work}/*/install.sh --uninstall"
  [[ ! -e ${home}/.local/share/obs-studio/plugins/${name} && ! -e ${home}/.config/obs-studio/plugins/${name} ]] \
    || fail 'install.sh --uninstall left files behind'
  [[ -f ${home}/.config/obs-studio/plugin_config/${name}/keep.json ]] || fail 'install.sh touched plugin_config'
  echo "Per-user install and uninstall checked on ${PRETTY_NAME}"
  exit 0
fi

so="${libdir}/obs-modules/plugins/${name}.so"
check_plugin "${so}"
check_link "${libdir}/obs-plugins/${name}.so" "${so}"
[[ -f /usr/share/obs/obs-modules/plugins/${name}/locale/en-US.ini ]] || fail 'data folder missing'
[[ -f /usr/share/obs/obs-modules/plugins/${name}/licenses/LICENSE-ffmpeg-LGPL-2.1.txt ]] || fail 'FFmpeg licence missing'
check_link "/usr/share/obs/obs-plugins/${name}" "/usr/share/obs/obs-modules/plugins/${name}"
echo "${family} package checked on ${PRETTY_NAME}"
