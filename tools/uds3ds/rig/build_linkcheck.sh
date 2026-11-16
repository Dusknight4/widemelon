#!/bin/bash
# builds linkcheck.exe (measures UdsLink's rough link) into $1 (default C:/wmbuild/uds3ds/linkcheck)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/c/wmbuild/uds3ds/linkcheck}
mkdir -p "$OUT"
export PATH=/c/msys64/ucrt64/bin:$PATH
M=$HERE/../../../src/frontend/qt_sdl
g++ -std=gnu++17 -O2 -static -I"$M" "$HERE/linkcheck.cpp" "$M/UdsLink.cpp" -o "$OUT/linkcheck.exe" -lws2_32 -lwinmm
echo built linkcheck.exe
