#!/usr/bin/env python3
# Copyright 2026 WideMelon contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# azahar_libs.py: Azahar's static libraries, ready to link into MultiMelon (MULTIMELON_3DS).
#
# Both projects bundle some of the same libraries (teakra, the DSi's and the 3DS's DSP; glad, the
# OpenGL loader; enet), in different versions. One program can't hold two definitions of a name,
# and a static link may even quietly take one project's definition for the other's code (two
# teakras with different class layouts). So every global name Azahar's libraries define that
# MultiMelon's link also defines gets "az_" in front, in Azahar's copies only: Azahar's code then
# uses its own. All of teakra's, glad's and enet's names are renamed whatever MultiMelon has.
#
# usage: azahar_libs.py --mm-build DIR --azahar-build DIR --out DIR [--nm nm] [--objcopy objcopy] [--ar ar]
#   --mm-build: a MultiMelon build without the 3DS core (its build.ninja says what multimelon.exe links)
#   --out: the renamed libraries (lib*.a), azahar-renames.txt (old new) and azahar-libs.txt (link order)

import argparse, os, re, subprocess, sys, tempfile

AZAHAR_LIBS = [  # in link order (dependents first)
    "src/core/libcitra_core.a", "src/video_core/libvideo_core.a", "src/audio_core/libaudio_core.a",
    "src/input_common/libinput_common.a", "src/network/libnetwork.a", "src/common/libcitra_common.a",
    "externals/dynarmic/src/dynarmic/libdynarmic.a", "externals/dynarmic/externals/zydis/libZydis.a",
    "externals/dynarmic/externals/zydis/zycore/libZycore.a", "externals/dynarmic/externals/mcl/src/libmcl.a",
    "externals/teakra/src/libteakra.a", "externals/libboost_serialization.a", "externals/libboost_iostreams.a",
    "externals/cryptopp/libcryptopp.a", "externals/libressl/ssl/libssl-54.a", "externals/libressl/crypto/libcrypto-51.a",
    "externals/fmt/libfmt.a", "externals/glad/libglad.a", "externals/enet/libenet.a", "externals/cubeb/libcubeb.a",
    "externals/soundtouch/libSoundTouch.a", "externals/faad2/libfaad2.a", "externals/lodepng/liblodepng.a",
    "externals/xxHash/cmake_unofficial/libxxhash.a", "externals/libzstd_seekable.a",
    "externals/zstd/build/cmake/lib/libzstd.a",
]
RENAME_ALL = ("libteakra.a", "libglad.a", "libenet.a")
# shared on purpose: the C++ runtime's template instances and the like, identical in both
KEEP = re.compile(r"^(_ZN?K?St|_ZSt|_ZNKSt|_ZN9__gnu_cxx|_ZNK9__gnu_cxx|_ZT[VISTH]St|_ZT[VISTH]N9__gnu_cxx|_ZGVZ.*St|__|\.)")


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, errors="replace").stdout


def defined(nm, path):
    # global names a file (archive or object) defines
    out = run([nm, "-g", "--defined-only", "-P", path])
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and not parts[0].endswith(":"):
            names.add(parts[0])
    return names


def mm_inputs(build):
    # what build.ninja says multimelon.exe links: its objects and libraries, -l resolved to lib*.a
    text = open(os.path.join(build, "build.ninja"), encoding="utf-8", errors="replace").read()
    m = re.search(r"^build multimelon\.exe[^:]*: \S+ (.*?)(?: \|.*)?$\n((?:  .*\n)*)", text, re.M)
    if not m:
        sys.exit("no multimelon.exe in " + build)
    files = m.group(1).split()
    libs = re.search(r"^  LINK_LIBRARIES = (.*)$", m.group(2), re.M)
    paths = re.search(r"^  LINK_PATH = (.*)$", m.group(2), re.M)
    search = [p[2:] for p in (paths.group(1).split() if paths else []) if p.startswith("-L")]
    search += ["C:/msys64/ucrt64/lib"]
    for item in (libs.group(1).split() if libs else []):
        if item.startswith("-l"):
            for d in search:
                cand = os.path.join(d, "lib" + item[2:] + ".a")
                if os.path.exists(cand):
                    files.append(cand)
                    break
        elif item.endswith((".a", ".obj", ".o")):
            files.append(item)
    out = []
    for f in files:
        p = f if os.path.isabs(f) else os.path.join(build, f)
        if os.path.exists(p) and not p.endswith(".dll.a"):
            out.append(p)
    return out


def archive_members(ar, lib):
    # a thin archive's members (paths of the objects); None for a normal archive
    with open(lib, "rb") as f:
        if f.read(8) != b"!<thin>\n":
            return None
    base = os.path.dirname(lib)
    return [m if os.path.isabs(m) else os.path.normpath(os.path.join(base, m)) for m in run([ar, "t", lib]).split("\n") if m]


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--mm-build", required=True)
    a.add_argument("--azahar-build", required=True)
    a.add_argument("--out", required=True)
    a.add_argument("--nm", default="nm")
    a.add_argument("--objcopy", default="objcopy")
    a.add_argument("--ar", default="ar")
    o = a.parse_args()
    os.makedirs(o.out, exist_ok=True)

    mine = set()
    for f in mm_inputs(o.mm_build):
        try:
            mine |= defined(o.nm, f)
        except subprocess.CalledProcessError:
            pass
    print("MultiMelon's link defines %d names" % len(mine))

    libs = [os.path.join(o.azahar_build, l) for l in AZAHAR_LIBS]
    missing = [l for l in libs if not os.path.exists(l)]
    if missing:
        sys.exit("missing: " + ", ".join(missing))
    renames = {}
    for lib in libs:
        names = defined(o.nm, lib)
        whole = os.path.basename(lib) in RENAME_ALL
        for n in names:
            if (whole or n in mine) and not KEEP.match(n):
                renames[n] = "az_" + n
    print("renaming %d names" % len(renames))
    mapfile = os.path.join(o.out, "azahar-renames.txt")
    with open(mapfile, "w", encoding="utf-8", newline="\n") as f:
        for n in sorted(renames):
            f.write("%s %s\n" % (n, renames[n]))

    order = []
    for lib in libs:
        dst = os.path.join(o.out, os.path.basename(lib))
        members = archive_members(o.ar, lib)
        src = lib
        if members is not None:
            # a normal archive of the thin one's objects, which objcopy can rewrite
            fd, src = tempfile.mkstemp(suffix=".a", dir=o.out)
            os.close(fd)
            os.remove(src)
            for i in range(0, len(members), 200):
                run([o.ar, "qc", src] + members[i:i + 200])
        run([o.objcopy, "--redefine-syms=" + mapfile, src, dst])
        if src != lib:
            os.remove(src)
        run([o.ar, "s", dst])
        order.append(dst)
    with open(os.path.join(o.out, "azahar-libs.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(order) + "\n")
    print("wrote %d libraries to %s" % (len(order), o.out))


if __name__ == "__main__":
    main()
