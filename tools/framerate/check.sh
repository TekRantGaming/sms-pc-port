#!/usr/bin/env bash
# Clock-independent timing checks; no ROM, GPU, or high-refresh monitor needed.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/framerate-tests
for arch in 32 64; do
  "${CXX:-g++}" -std=c++03 -m"$arch" -Isrc/port_include tools/framerate/test.cpp -o "build/framerate-tests/timing-$arch"
  "build/framerate-tests/timing-$arch"
done
