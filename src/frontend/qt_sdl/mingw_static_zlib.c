// WideMelon: static MinGW builds.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// MSYS2's static libarchive is compiled against zlib's DLL interface, so it
// calls zlib through import pointers (__imp_inflate and so on). A static zlib
// has no such pointers; provide them, pointing at the statically linked
// functions.

#include <zlib.h>

#define ZLIB_IMPORT(name) void* __imp_##name = (void*)name

ZLIB_IMPORT(crc32);
ZLIB_IMPORT(inflate);
ZLIB_IMPORT(inflateEnd);
ZLIB_IMPORT(inflateInit_);
ZLIB_IMPORT(inflateInit2_);
ZLIB_IMPORT(inflateReset);
ZLIB_IMPORT(inflateSetDictionary);
