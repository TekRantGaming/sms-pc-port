#!/usr/bin/env bash
# ./build.sh [GMSE01 disc image]
#
# Builds the port for this computer into build/<os>-<arch>/:
#   Linux    build/linux-32/sms       (SMS_ARCH=64: build/linux-64/sms)
#   macOS    build/macos-64/sms       (x86_64, runs under Rosetta on Apple Silicon)
#   Windows  build/windows-64/sms.exe (MSYS2 MINGW64 shell, or build.cmd)
# With a disc image (the argument, SMS_DISC_IMAGE, or the one image in rom/)
# it also builds a copy that has the game's files inside and needs no image:
# sms-standalone (sms-standalone.exe on Windows, SMS.app on macOS).
# JOBS=n limits parallel compiler jobs. See BUILD.md.
set -euo pipefail

cd "$(dirname "$0")"
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
fi
source tools/common.sh
sms_detect_os
sms_migrate_legacy
sms_legacy_notes
sms_select_arch build
bdir=$sms_build_dir
cmake_args=()

need() {
  local missing=() p
  for p in "$@"; do
    command -v "$p" >/dev/null 2>&1 || missing+=("$p")
  done
  if (( ${#missing[@]} )); then
    sms_die "Missing ${missing[*]}. See BUILD.md for the $sms_os prerequisites."
  fi
}

setup_linux() {
  need git cmake make patch python3 objcopy g++
  if [[ "$sms_arch" == 32 ]] && ! echo 'int main(){return 0;}' | "${CXX:-g++}" -m32 -x c++ - -o /dev/null 2>/dev/null; then
    sms_die "g++ -m32 does not link: install the 32-bit packages (BUILD.md#linux), or build 64-bit with SMS_ARCH=64 ./build.sh."
  fi
}

# Homebrew LLVM (clang targeting x86_64, and llvm-objcopy) and a universal
# SDL2.framework (Homebrew's sdl2 is arm64-only on Apple Silicon).
setup_macos() {
  need git cmake make patch python3 clang++ curl
  if [[ "$(uname -m)" == arm64 ]] && ! /usr/bin/arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
    sms_die "Rosetta 2 is required on Apple Silicon (the game is an x86_64 program): softwareupdate --install-rosetta"
  fi

  local objcopy="" c
  for c in "${SMS_LLVM_BIN:-}/llvm-objcopy" "$(brew --prefix llvm 2>/dev/null)/bin/llvm-objcopy" \
           /opt/homebrew/opt/llvm/bin/llvm-objcopy /usr/local/opt/llvm/bin/llvm-objcopy; do
    if [[ -x "$c" ]]; then objcopy=$c; break; fi
  done
  [[ -n "$objcopy" ]] || objcopy=$(command -v llvm-objcopy || true)
  [[ -n "$objcopy" ]] || sms_die "Missing llvm-objcopy (renames the game's operator new/delete): brew install llvm"
  local llvm_bin cc cxx
  llvm_bin=$(dirname "$objcopy")
  export PATH="$llvm_bin:$PATH"
  cc=$(command -v clang)
  cxx=$(command -v clang++)

  # build/deps/ survives deleting build/macos-64/.
  local sdl_ver=2.30.11
  sdl2_fw="$PWD/build/deps/SDL2.framework"
  if [[ ! -f "$sdl2_fw/SDL2" ]]; then
    echo "Fetching the universal SDL2.framework $sdl_ver into build/deps/ ..."
    mkdir -p build/deps
    local dmg="build/deps/SDL2-$sdl_ver.dmg" mnt
    if [[ ! -f "$dmg" ]]; then
      curl -fsSL -o "$dmg.part" "https://github.com/libsdl-org/SDL/releases/download/release-$sdl_ver/SDL2-$sdl_ver.dmg"
      mv "$dmg.part" "$dmg"
    fi
    mnt=$(hdiutil attach "$dmg" -nobrowse -readonly | awk -F'\t' '/\/Volumes\//{print $NF; exit}')
    [[ -n "$mnt" && -d "$mnt/SDL2.framework" ]] || sms_die "Could not mount $dmg"
    rm -rf "$sdl2_fw"
    cp -R "$mnt/SDL2.framework" "$sdl2_fw"
    hdiutil detach "$mnt" -quiet || true
  fi

  # A CMake tree configured for another architecture cannot be reused.
  if [[ -f "$bdir/CMakeCache.txt" ]] && ! grep -q 'CMAKE_OSX_ARCHITECTURES:STRING=x86_64' "$bdir/CMakeCache.txt"; then
    echo "Removing $bdir (it was not configured for x86_64)"
    rm -rf "$bdir"
  fi
  cmake_args+=(-DCMAKE_OSX_ARCHITECTURES=x86_64
               -DCMAKE_C_COMPILER="$cc"
               -DCMAKE_CXX_COMPILER="$cxx"
               -DSMS_SDL2_FRAMEWORK="$sdl2_fw")
}

setup_windows() {
  need git cmake ninja patch python
  # FindPython otherwise prefers a registered system installation, even when
  # the portable MSYS2 Python is first on PATH.
  cmake_args+=(-G Ninja -DPython3_EXECUTABLE="$(cygpath -m "$(command -v python)")")
}

"setup_$sms_os"

disc="${1:-${SMS_DISC_IMAGE:-}}"
[[ -n "$disc" ]] || disc=$(sms_find_rom)
if [[ -n "$disc" ]]; then
  [[ -f "$disc" ]] || sms_die "Disc image not found: $disc"
  if [[ "$sms_os" == windows ]]; then
    disc="$(cd "$(dirname "$disc")" && pwd -W)/$(basename "$disc")"
  else
    disc="$(cd "$(dirname "$disc")" && pwd)/$(basename "$disc")"
  fi
fi

git submodule update --init decomp
cmake -S . -B "$bdir" ${cmake_args[@]+"${cmake_args[@]}"} \
  -DSMS_ARCH="$sms_arch" -DSMS_GX_BUILD_TESTS=OFF -DSMS_BUNDLE_DISC="$disc"
cmake --build "$bdir" --target sms --parallel "$(sms_jobs)"
if [[ "$sms_os" == macos ]]; then
  # The executable finds SDL2.framework beside itself (@executable_path).
  rm -rf "$bdir/SDL2.framework"
  cp -R "$sdl2_fw" "$bdir/SDL2.framework"
fi

exe="$bdir/sms$sms_exe_suffix"
echo
echo "Built $exe ($sms_os, $sms_arch-bit)"
if [[ -n "$disc" ]]; then
  cmake --build "$bdir" --target sms_standalone
  if [[ "$sms_os" == macos ]]; then
    echo "Built $bdir/SMS.app with the game from $disc inside"
  else
    echo "Built $(sms_standalone_exe) with the game from $disc inside"
  fi
  echo "Play: ./run.sh"
else
  echo "Play: ./run.sh /path/to/your/GMSE01.iso   (or put the image in rom/ and run ./run.sh)"
fi
