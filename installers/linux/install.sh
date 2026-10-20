#!/usr/bin/env bash
# Trigglow Dynamic Delay for OBS Studio: per-user install on Linux, from the
# .tar.xz release (no root, nothing outside your home folder).
#
#   ./install.sh              install or update
#   ./install.sh --uninstall  remove it again
#
# Installs the plugin where OBS 33 and later look for your own plugins:
#   ${XDG_DATA_HOME:-~/.local/share}/obs-studio/plugins/obs-trigglow-dynamic-delay/
#       obs-trigglow-dynamic-delay.so
#       data/
# and links it into the legacy folder OBS 31 and 32 read (OBS 33 skips that
# copy as a duplicate):
#   ${XDG_CONFIG_HOME:-~/.config}/obs-studio/plugins/obs-trigglow-dynamic-delay/
#       bin/64bit/obs-trigglow-dynamic-delay.so -> the .so above
#       data -> the data folder above
#
# Your settings and sign-in live in ~/.config/obs-studio/plugin_config/: never
# touched, not even by --uninstall.
# The Flatpak OBS is not supported (it keeps its plugins elsewhere and runs
# them in its own runtime).

set -euo pipefail

name=obs-trigglow-dynamic-delay
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source_dir="${here}/${name}"
data_home=${XDG_DATA_HOME:-${HOME}/.local/share}
config_home=${XDG_CONFIG_HOME:-${HOME}/.config}
target="${data_home}/obs-studio/plugins/${name}"
legacy="${config_home}/obs-studio/plugins/${name}"

say() { printf '%s\n' "$*"; }
die() {
  printf 'install.sh: %s\n' "$*" >&2
  exit 1
}

if [[ ${EUID} -eq 0 ]]; then
  die "run it as your own user, not as root or with sudo: it installs into your home folder. For all users, install the .deb, .rpm or .pkg.tar.zst package instead."
fi
[[ -n ${HOME:-} && -d ${HOME} ]] || die 'HOME is not set'

# Only ever our own two folders under obs-studio/plugins/.
remove_installed() {
  [[ ${target} == */obs-studio/plugins/${name} && ${legacy} == */obs-studio/plugins/${name} ]] \
    || die "refusing to remove unexpected paths: ${target}, ${legacy}"
  rm -rf -- "${target}"
  rm -rf -- "${legacy}/bin" "${legacy}/data"
  rmdir -- "${legacy}" 2> /dev/null || true
}

case ${1:-} in
  --uninstall)
    remove_installed
    say "Trigglow Dynamic Delay removed from ${target} and ${legacy}."
    say 'Your settings in ~/.config/obs-studio/plugin_config/ are kept.'
    exit 0
    ;;
  '') ;;
  -h | --help)
    sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
  *) die "unknown option ${1} (use --uninstall or nothing)" ;;
esac

[[ -f ${source_dir}/${name}.so && -d ${source_dir}/data ]] \
  || die "${source_dir} is incomplete: extract the whole .tar.xz and run install.sh from it"

if pgrep -x obs > /dev/null 2>&1; then
  say 'OBS Studio is open: close it before using the plugin, it is loaded when OBS starts.'
fi

remove_installed
mkdir -p -- "${target}" "${legacy}/bin/64bit"
cp -R -- "${source_dir}/." "${target}/"
chmod 0644 "${target}/${name}.so"
ln -s -- "${target}/${name}.so" "${legacy}/bin/64bit/${name}.so"
ln -s -- "${target}/data" "${legacy}/data"

say "Trigglow Dynamic Delay installed in ${target}"
say "(and linked into ${legacy} for OBS 31 and 32)."
for system_copy in /usr/lib/*/obs-modules/plugins/${name}.so /usr/lib64/obs-modules/plugins/${name}.so \
  /usr/lib/obs-modules/plugins/${name}.so; do
  if [[ -e ${system_copy} ]]; then
    say "Note: a system package of the plugin is installed too (${system_copy}). Keep only one:"
    say '  remove the package, or run ./install.sh --uninstall.'
    break
  fi
done
say 'Start OBS Studio: the "Trigglow Dynamic Delay" dock is under View > Docks.'
