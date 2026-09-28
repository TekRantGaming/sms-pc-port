#!/usr/bin/env bash
# ./run.sh [GMSE01 disc image or extracted files/ folder] [--headless]
#
# Runs the build for this computer (build/<os>-<arch>/, see build.sh).
# The game comes from, in order: the argument, SMS_DISC_IMAGE or
# SMS_DISC_ROOT; else the standalone build (sms-standalone / SMS.app) when
# it exists; else the one disc image in rom/.
# SMS_ARCH=32|64 picks the build when both exist. See README.md for options.
set -euo pipefail

cd "$(dirname "$0")"
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
fi
source tools/common.sh
sms_detect_os
sms_migrate_legacy
sms_legacy_notes
sms_select_arch run
exe="$sms_build_dir/sms$sms_exe_suffix"
standalone=$(sms_standalone_exe)

run_game() {
  if [[ "$sms_os" == linux && "$sms_arch" == 32 && -n "${SMS_LINUX32_ROOT:-}" ]]; then
    export SMS_GAME_EXECUTABLE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    exec "$SMS_LINUX32_ROOT/i686-buildroot-linux-gnu/sysroot/lib/ld-linux.so.2" \
      --library-path "$SMS_LINUX32_LIBRARY_PATH" "$@"
  fi
  exec "$@"
}

have_disc=0
for arg in "$@"; do
  if [[ "$arg" != -* ]]; then
    have_disc=1
    [[ -e "$arg" ]] || sms_die "Disc image or extracted disc folder not found: $arg"
  fi
done
if [[ -n "${SMS_DISC_IMAGE:-}" || -n "${SMS_DISC_ROOT:-}" ]]; then
  have_disc=1
fi

if (( have_disc == 0 )) && [[ -x "$standalone" ]]; then
  if [[ -e "$exe" && "$exe" -nt "$standalone" ]]; then
    echo "Note: $standalone is older than $exe; ./build.sh with your disc image refreshes it." >&2
  fi
  run_game "$standalone" "$@"
fi
[[ -e "$exe" ]] || sms_die "No build in $sms_build_dir/: run ./build.sh first."
if (( have_disc == 0 )); then
  rom=$(sms_find_rom)
  [[ -n "$rom" ]] || sms_die "No game: put your GMSE01 disc image (.iso, .gcm or .ciso) in rom/, or pass its path: ./run.sh /path/to/GMSE01.iso"
  set -- "$rom" "$@"
fi
run_game "$exe" "$@"
