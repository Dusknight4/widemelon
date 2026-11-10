#!/bin/bash
# builds the PC test of uds_core (MSYS2 UCRT64); the crypto self-test runs at its start
#   fake3ds.exe: uds_core over MultiMelon's UdsLink, driven like udstest (see fake3ds.cpp)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
M=$HERE/../../../src/frontend/qt_sdl
OUT=${1:-/c/wmbuild/uds3ds/coretest}
export PATH=/c/msys64/ucrt64/bin:$PATH
mkdir -p "$OUT"
g++ -std=c++17 -O2 -Wall -Wextra -I"$HERE" -I"$M" "$HERE/fake3ds.cpp" "$HERE/uds_core.cpp" "$HERE/uds_crypto.cpp" "$M/UdsLink.cpp" \
    -static -lws2_32 -liphlpapi -o "$OUT/fake3ds.exe"
echo "built $OUT/fake3ds.exe"
