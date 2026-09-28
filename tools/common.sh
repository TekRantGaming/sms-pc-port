# Shared by build.sh, run.sh and clean.sh (bash, run from the repository root): which
# host this is, which word size to build or run, where that build lives, and
# where the disc image is.
#
#   build/<os>-<arch>/   one CMake tree per host and word size
#                        (linux-32, linux-64, macos-64, windows-64)
#   build/deps/          downloaded dependencies (SDL2.framework on macOS)
#   rom/                 the user's GMSE01 disc image (never committed)

sms_die() {
  echo "$*" >&2
  exit 1
}

sms_image_exts=(iso gcm ciso ISO GCM CISO)

# Sets sms_os (linux, macos, windows), sms_arches (supported word sizes,
# default first) and sms_exe_suffix.
sms_detect_os() {
  sms_exe_suffix=""
  case "$(uname -s)" in
    Linux)
      sms_os=linux
      sms_arches=(32 64)
      ;;
    Darwin)
      # x86_64 under Rosetta; native arm64 cannot map memory below 4 GiB.
      sms_os=macos
      sms_arches=(64)
      ;;
    MINGW* | MSYS*)
      if [[ "${MSYSTEM:-}" != MINGW64 && "${MSYSTEM:-}" != MINGW32 ]]; then
        sms_die "Use the MSYS2 MINGW64 shell (or build.cmd / run.cmd from PowerShell). See BUILD.md#windows-msys2-mingw64."
      fi
      sms_os=windows
      if [[ "${MSYSTEM:-}" == MINGW64 ]]; then
        sms_arches=(64)
        [[ "${SMS_WINDOWS_32_CROSS:-}" == 1 ]] && sms_arches+=(32)
      else sms_arches=(32); fi
      sms_exe_suffix=.exe
      ;;
    *)
      sms_die "Unsupported system $(uname -s). See BUILD.md."
      ;;
  esac
}

sms_dir_for() { echo "build/$sms_os-$1"; }

# Sets sms_arch and sms_build_dir. SMS_ARCH=32|64 chooses; otherwise the
# host's default, except that run.sh takes the other word size when only
# that one has been built.
sms_select_arch() {
  local mode=$1 a
  if [[ -n "${SMS_ARCH:-}" ]]; then
    for a in "${sms_arches[@]}"; do
      if [[ "$a" == "$SMS_ARCH" ]]; then
        sms_arch=$a
        sms_build_dir=$(sms_dir_for "$a")
        return
      fi
    done
    case "$sms_os-$SMS_ARCH" in
      macos-32) sms_die "macOS cannot run 32-bit programs; the macOS build is 64-bit only (unset SMS_ARCH)." ;;
      windows-*) sms_die "SMS_ARCH=$SMS_ARCH needs the matching MSYS2 MINGW$SMS_ARCH shell." ;;
      *) sms_die "SMS_ARCH must be one of: ${sms_arches[*]}" ;;
    esac
  fi
  sms_arch=${sms_arches[0]}
  if [[ "$mode" == run && ! -e "$(sms_dir_for "$sms_arch")/sms$sms_exe_suffix" ]]; then
    for a in "${sms_arches[@]}"; do
      if [[ -e "$(sms_dir_for "$a")/sms$sms_exe_suffix" ]]; then
        sms_arch=$a
        break
      fi
    done
  fi
  sms_build_dir=$(sms_dir_for "$sms_arch")
}

# The executable with the disc's files bundled in (built when build.sh is
# given a disc image): sms-standalone, or SMS.app on macOS.
sms_standalone_exe() {
  if [[ "$sms_os" == macos ]]; then
    echo "$sms_build_dir/SMS.app/Contents/MacOS/sms"
  else
    echo "$sms_build_dir/sms-standalone$sms_exe_suffix"
  fi
}

# Prints the single disc image in rom/, nothing if there is none; fails if
# there are several.
sms_find_rom() {
  local images=() e f
  for e in "${sms_image_exts[@]}"; do
    for f in rom/*."$e"; do
      [[ -f "$f" ]] && images+=("$f")
    done
  done
  if (( ${#images[@]} > 1 )); then
    sms_die "rom/ holds more than one disc image (${images[*]}); keep one there or pass the path."
  fi
  if (( ${#images[@]} == 1 )); then
    echo "${images[0]}"
  fi
}

# Earlier layouts kept a rom/ folder inside each build directory
# (build/, build-64/, build-mac/, build32/bin/) and SDL2.framework in
# third_party/. Move the user's image and the framework to where they live
# now.
sms_legacy_rom_dirs=(build/rom build-64/rom build-mac/rom build32/bin/rom)
sms_legacy_build_dirs=(build-64 build-mac build32)
sms_migrate_legacy() {
  local d e f
  for d in "${sms_legacy_rom_dirs[@]}"; do
    for e in "${sms_image_exts[@]}"; do
      for f in "$d"/*."$e"; do
        [[ -f "$f" ]] || continue
        mkdir -p rom
        if [[ -e "rom/$(basename "$f")" ]]; then
          echo "Note: $f has the same name as rom/$(basename "$f"); if it is a second copy, it can be deleted." >&2
        else
          mv "$f" rom/
          echo "Moved $f to rom/ (the one place build.sh and run.sh look for it now)." >&2
        fi
      done
    done
  done
  if [[ -f third_party/SDL2.framework/SDL2 && ! -e build/deps/SDL2.framework ]]; then
    mkdir -p build/deps
    mv third_party/SDL2.framework build/deps/
    rmdir third_party 2>/dev/null || true
    echo "Moved third_party/SDL2.framework to build/deps/." >&2
  fi
}

# Points out build output of earlier layouts, which nothing uses any more.
sms_legacy_notes() {
  local d old=()
  for d in "${sms_legacy_build_dirs[@]}"; do
    [[ -d "$d" ]] && old+=("$d/")
  done
  [[ -f build/CMakeCache.txt ]] && old+=("the old build tree at the top of build/")
  if (( ${#old[@]} )); then
    echo "Note: builds now go to build/<os>-<arch>/; ./clean.sh deletes the old build output (${old[*]})." >&2
  fi
}

sms_jobs() {
  if [[ -n "${JOBS:-}" ]]; then
    echo "$JOBS"
  elif command -v nproc >/dev/null 2>&1; then
    nproc
  else
    sysctl -n hw.ncpu 2>/dev/null || echo 4
  fi
}
