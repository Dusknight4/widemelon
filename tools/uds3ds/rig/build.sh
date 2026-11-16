#!/bin/bash
# builds rig3ds.exe: rig3ds.cpp + MultiMelon's UdsLink/ThreeDSNet + Azahar's core libraries (build-core)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/c/wmbuild/uds3ds/rig}
mkdir -p "$OUT" && cd "$OUT"
export PATH=/c/msys64/ucrt64/bin:$PATH
A=/c/wmbuild/azahar/src/azahar-unified-source-2126.2
B=/c/wmbuild/azahar/build-core
M=/c/wmbuild/wt-3ds/src/frontend/qt_sdl
DEFS="-DBOOST_ALL_NO_LIB -DBOOST_ASIO_DISABLE_CONCEPTS -DBOOST_DATE_TIME_NO_LIB -DBOOST_ERROR_CODE_HEADER_ONLY -DBOOST_NO_CXX98_FUNCTION_BASE -DBOOST_REGEX_NO_LIB -DBOOST_SYSTEM_NO_LIB -DCITRA_HAS_SSE42 -DENABLE_OPENGL -DENABLE_SDL2 -DENABLE_SOFTWARE_RENDERER -DHAVE_CUBEB -DMICROPROFILE_ENABLED=0 -D_FILE_OFFSET_BITS=64"
INC="-I/c/msys64/ucrt64/include/SDL2 -I$M -I$B/src -I$A/src -I$A/externals/boost -I$A/externals/fmt/include -I$A/externals/dynarmic/src -I$A/externals/microprofile -I$A/externals/dds-ktx -I$A/externals/glad/include -I$A/externals/lodepng/lodepng"
mkdir -p obj
g++ -std=gnu++20 -O2 -msse4.2 $DEFS $INC -c "$HERE/rig3ds.cpp" -o obj/rig3ds.o &
g++ -std=gnu++20 -O2 -msse4.2 $DEFS $INC -c $M/ThreeDSNet.cpp -o obj/ThreeDSNet.o &
g++ -std=gnu++17 -O2 -c $M/UdsLink.cpp -o obj/UdsLink.o &
wait
LIBS=""
for l in src/core/libcitra_core.a src/video_core/libvideo_core.a src/audio_core/libaudio_core.a src/input_common/libinput_common.a \
         src/network/libnetwork.a src/common/libcitra_common.a externals/dynarmic/src/dynarmic/libdynarmic.a \
         externals/dynarmic/externals/zydis/libZydis.a externals/dynarmic/externals/zydis/zycore/libZycore.a \
         externals/dynarmic/externals/mcl/src/libmcl.a externals/teakra/src/libteakra.a externals/libboost_serialization.a \
         externals/libboost_iostreams.a externals/cryptopp/libcryptopp.a externals/libressl/ssl/libssl-54.a \
         externals/libressl/crypto/libcrypto-51.a externals/fmt/libfmt.a externals/glad/libglad.a externals/enet/libenet.a \
         externals/cubeb/libcubeb.a externals/soundtouch/libSoundTouch.a externals/faad2/libfaad2.a \
         externals/lodepng/liblodepng.a externals/xxHash/cmake_unofficial/libxxhash.a externals/libzstd_seekable.a \
         externals/zstd/build/cmake/lib/libzstd.a; do LIBS="$LIBS $B/$l"; done
g++ -static -o rig3ds.exe obj/rig3ds.o obj/ThreeDSNet.o obj/UdsLink.o -Wl,--start-group $LIBS -Wl,--end-group \
    $(pkg-config --static --libs sdl2 | sed 's/-mwindows//; s/-lSDL2main//') -lopengl32 -lwinmm -lws2_32 -lpsapi -limm32 -lversion -lbcrypt -lcrypt32 -lole32 -luuid -lavrt -lksuser -lssp -liphlpapi
echo built rig3ds.exe
