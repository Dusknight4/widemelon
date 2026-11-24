#!/usr/bin/env bash
# Copyright (C) 2026 WideMelon contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# azahar-core.sh WORKDIR ARCH: Azahar's emulation core for MultiMelon's 3DS games (MULTIMELON_3DS), as the
# macOS packages build it. Downloads Azahar's unified source (checked against its SHA-256), applies
# patches/azahar (0001: the frontend carries local wireless; 0003: Vulkan offscreen, MoltenVK from the app
# bundle), and builds the core libraries only (no Qt, no app) with the Vulkan renderer: Azahar has no
# OpenGL renderer on macOS. ARCH: arm64 or x86_64.
#
# Leaves WORKDIR/src (the patched source, for its headers), WORKDIR/build (the libraries,
# compile_commands.json) and WORKDIR/build/externals/MoltenVK (Azahar's MoltenVK download, whose
# libMoltenVK.dylib goes into the app).
set -euo pipefail

work=$1
arch=$2
repo=$(cd "$(dirname "$0")/.." && pwd)
version=2126.2
sha256=85c8b56fac703df04720125044b8ff887997911858a6cde545dfb1107ba4cd26
archive="azahar-unified-source-$version.tar.xz"
url="https://github.com/azahar-emu/azahar/releases/download/$version/$archive"

mkdir -p "$work"
cd "$work"
if [[ ! -f "$archive" ]] || ! echo "$sha256  $archive" | shasum -a 256 -c --status; then
    curl -fL --retry 3 -o "$archive" "$url"
fi
echo "$sha256  $archive" | shasum -a 256 -c

rm -rf src
mkdir src
tar -xJf "$archive" -C src --strip-components=1
for p in 0001-uds-link.patch 0003-vulkan-offscreen.patch; do
    patch -d src -p1 --forward < "$repo/patches/azahar/$p"
done

cmake -G Ninja -S src -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DENABLE_QT=OFF -DENABLE_SDL2=OFF -DENABLE_ROOM=OFF -DENABLE_SCRIPTING=OFF -DENABLE_WEB_SERVICE=OFF \
    -DENABLE_OPENAL=OFF -DENABLE_LIBUSB=OFF -DENABLE_TESTS=OFF -DENABLE_DISCORD_RPC=OFF \
    -DENABLE_LTO=OFF -DCITRA_WARNINGS_AS_ERRORS=OFF -DENABLE_VULKAN=ON \
    -DCLANG_FORMAT=/usr/bin/false
ninja -C build citra_core video_core audio_core input_common network citra_common
# (the objects are in the libraries: a smaller cache)
find build -name '*.o' -delete

moltenvk=$(find build/externals/MoltenVK -path '*dynamic/dylib/macOS/libMoltenVK.dylib' | head -n 1)
if [[ -z "$moltenvk" ]]; then
    echo "No libMoltenVK.dylib in Azahar's build" >&2
    exit 1
fi
lipo -info "$moltenvk"
echo "Azahar's core is in $work/build; MoltenVK: $work/$moltenvk"
