#!/usr/bin/env bash
# Run against sources produced by CMake's decomp-patch application.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-"$root/build/linux-64"}
arch=${2:-64}
out="$build/audio-port-test"
mkdir -p "$out"
includes=(-I"$root/src/port_include" -I"$build/patched/libs/JSystem/include"
          -I"$root/decomp/libs/JSystem/include" -I"$root/decomp/libs/dolphin/include")
queue="$build/patched/libs/JSystem/src/JAudio/JASystem/JASCmdStack.cpp"
source="$build/patched/libs/JSystem/src/JAudio/JAInterface/JAISystemInterface.cpp"
[[ -f "$source" ]] || { echo 'Configure CMake first to apply the audio patch.' >&2; exit 1; }
"${CXX:-g++}" -m"$arch" -std=gnu++11 -O2 -g -fno-strict-aliasing -fpermissive \
    -DTARGET_PC -DGEKKO -ffunction-sections -fdata-sections "${includes[@]}" \
    "$root/tools/audio-port/pending_commands.cpp" "$source" \
    "$queue" \
    -Wl,--gc-sections -o "$out/pending_commands"
"$out/pending_commands"

"${CXX:-g++}" -m"$arch" -std=gnu++11 -O2 -g -fno-strict-aliasing -fpermissive \
    -DTARGET_PC -DGEKKO -ffunction-sections -fdata-sections "${includes[@]}" \
    "$root/tools/audio-port/port_parameters.cpp" "$source" \
    "$queue" \
    "$root/decomp/libs/JSystem/src/JAudio/JASystem/JASOuterParam.cpp" \
    -no-pie -Wl,--gc-sections -o "$out/port_parameters"
"$out/port_parameters" parameters
"$out/port_parameters" interrupt
"$out/port_parameters" rebind
