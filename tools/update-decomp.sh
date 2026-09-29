#!/usr/bin/env bash
# tools/update-decomp.sh [--64] [--no-build] [REF]
#
# Moves the decomp/ submodule to REF (default origin/main, fetched first),
# checks that every decomp-patches/*.patch still applies in order, exactly
# (no failed hunk, no fuzz), and builds the port 32-bit (with --64, 64-bit
# too). The submodule stays on the new commit, staged, only when everything
# passes; otherwise the old pin is restored. JOBS=n limits compiler jobs.
set -euo pipefail

cd "$(dirname "$0")/.."
if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
fi
source tools/common.sh
sms_detect_os

ref=origin/main
arches=(32)
build=1
while (( $# )); do
  case "$1" in
    --64) arches+=(64) ;;
    --no-build) build=0 ;;
    -*) sms_die "Unknown option $1 (see --help)" ;;
    *) ref=$1 ;;
  esac
  shift
done
if [[ "$sms_os" != linux ]]; then
  arches=("${sms_arches[@]}")
fi

[[ -e decomp/.git ]] || sms_die "decomp/ is not checked out: git submodule update --init decomp"
if [[ -n "$(git -C decomp status --porcelain --untracked-files=no)" ]]; then
  sms_die "decomp/ has local changes; commit or remove them first."
fi

old=$(git -C decomp rev-parse HEAD)
old_index=$(git ls-files --stage decomp | awk '{print $2}')

echo "Fetching the decomp remote ..."
git -C decomp fetch --quiet origin
if [[ "$ref" == origin/* ]]; then
  git -C decomp fetch --quiet origin "${ref#origin/}" || true
fi
new=$(git -C decomp rev-parse --verify --quiet "$ref^{commit}") \
  || sms_die "Unknown decomp ref $ref"
echo "decomp: ${old:0:12} -> ${new:0:12} ($ref, $(git -C decomp rev-list --count "$old..$new" 2>/dev/null || echo "?") new commits)"

restore() {
  git -C decomp checkout --quiet --detach "$old"
  git update-index --cacheinfo "160000,$old_index,decomp"
}
failed=()
summary=()

git -C decomp checkout --quiet --detach "$new"
git update-index --cacheinfo "160000,$new,decomp"

# Apply the patches in name order to copies of the files they touch, as
# cmake/patches.cmake does at configure time.
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
bad_patches=()
n=0
for p in decomp-patches/*.patch; do
  n=$((n + 1))
  missing=()
  while IFS= read -r f; do
    if [[ ! -e "$scratch/$f" ]]; then
      mkdir -p "$scratch/$(dirname "$f")"
      if [[ -e "decomp/$f" ]]; then
        cp "decomp/$f" "$scratch/$f"
      else
        : > "$scratch/$f"
        missing+=("$f")
      fi
    fi
  done < <(sed -n 's#^+++ b/\([^\t ]*\).*#\1#p' "$p")
  if (( ${#missing[@]} )); then
    # A file the decomp moved (e.g. the libraries' move to libs/<name>/src
    # and libs/<name>/include): the patch's paths need rewriting.
    bad_patches+=("$(basename "$p") (not in the decomp: ${missing[*]})")
    for f in "${missing[@]}"; do rm -f "$scratch/$f"; done
    continue
  fi
  if out=$(patch -p1 --dry-run -d "$scratch" -i "$PWD/$p" 2>&1); then
    patch -p1 --quiet -d "$scratch" -i "$PWD/$p" >/dev/null
    if grep -q fuzz <<<"$out"; then
      bad_patches+=("$(basename "$p") (applies only with fuzz; check where each hunk lands)")
    fi
  else
    bad_patches+=("$(basename "$p") ($(grep -c "^Hunk #.*FAILED" <<<"$out") hunk(s) failed)")
  fi
done
if (( ${#bad_patches[@]} )); then
  failed+=("patches")
  summary+=("patches: ${#bad_patches[@]} of $n do not apply cleanly:")
  for b in "${bad_patches[@]}"; do summary+=("  $b"); done
else
  summary+=("patches: all $n apply cleanly")
fi

if (( build )) && (( ${#failed[@]} == 0 )); then
  for a in "${arches[@]}"; do
    bdir=$(sms_dir_for "$a")
    log="$scratch/build-$a.log"
    echo "Building $a-bit in $bdir ..."
    if [[ -f "$bdir/CMakeCache.txt" ]]; then
      ok=1
      { cmake -S . -B "$bdir" -DSMS_ARCH="$a" -DSMS_GX_BUILD_TESTS=OFF \
          && cmake --build "$bdir" --target sms --parallel "$(sms_jobs)"; } > "$log" 2>&1 || ok=0
    else
      ok=1
      SMS_ARCH=$a ./build.sh > "$log" 2>&1 || ok=0
    fi
    if (( ok )); then
      summary+=("build $a-bit: ok ($bdir/sms$sms_exe_suffix)")
    else
      failed+=("build $a-bit")
      mkdir -p build
      cp "$log" "build/update-decomp-$a.log"
      summary+=("build $a-bit: FAILED (log: build/update-decomp-$a.log)")
      while IFS= read -r l; do summary+=("  $l"); done \
        < <(grep -E "error|undefined reference|multiple definition" "$log" | head -15)
    fi
  done
elif (( build )); then
  summary+=("builds: skipped (the patches must apply first)")
fi

echo
echo "== update-decomp summary"
for s in "${summary[@]}"; do echo "$s"; done
if (( ${#failed[@]} )); then
  restore
  # Rerun CMake so the build folders' patched copies match the old decomp.
  for a in "${arches[@]}"; do
    bdir=$(sms_dir_for "$a")
    [[ -f "$bdir/CMakeCache.txt" ]] && cmake -S . -B "$bdir" > /dev/null 2>&1 || true
  done
  echo "FAILED (${failed[*]}): decomp/ is back on ${old:0:12}."
  exit 1
fi
echo "OK: decomp/ is on ${new:0:12}, staged. Commit the new pin with:"
echo "  git commit -m \"Move the decomp to ${new:0:7}\" decomp"
