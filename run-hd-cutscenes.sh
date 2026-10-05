#!/usr/bin/env bash
# Run the game with an optional locally generated HD cutscene pack.
set -euo pipefail
cd "$(dirname "$0")"
pack=${SMS_HD_CUTSCENES:-"$PWD/mods/hd-cutscenes"}
if [[ ! -d "$pack/files/data" ]] || ! compgen -G "$pack/files/data/*.thp" >/dev/null; then
  echo "HD cutscenes are missing from $pack/files/data." >&2
  echo "See docs/HD-CUTSCENES.md to generate the pack from your disc." >&2
  exit 1
fi
export SMS_MOD="${SMS_MOD:+$SMS_MOD;}$pack"
export SMS_SKIP_MOVIES=0
exec ./run.sh "$@"
