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
#                       [--ranlib ranlib] [--macho]
#   --mm-build: a MultiMelon build without the 3DS core (its build.ninja says what the program links:
#     multimelon.exe, or on macOS MultiMelon.app's executable)
#   --out: the renamed libraries (lib*.a), azahar-renames.txt (old new), azahar-libs.txt (link order) and
#     azahar-defines.txt (the definitions Azahar's core was compiled with, which MultiMelon's files that
#     use Azahar's headers need too: some of its structures depend on them)
#   --macho: macOS (Mach-O) objects, whose names have an extra "_" in front. Use LLVM's tools there
#     (llvm-nm, llvm-objcopy) and Apple's ranlib.

import argparse, json, os, re, shlex, subprocess, sys, tempfile

AZAHAR_LIBS = [  # in link order (dependents first); those a build doesn't make (Zydis on ARM) are left out
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
REQUIRED = ("libcitra_core.a", "libvideo_core.a", "libcitra_common.a", "libdynarmic.a")
# libraries in the build that aren't Azahar's core: test frameworks, and MoltenVK's own copies (Azahar
# loads MoltenVK as a library at run time)
SKIP = re.compile(r"(catch2|gtest|gmock|benchmark|SDL2main|MoltenVK)", re.I)
RENAME_ALL = ("libteakra.a", "libglad.a", "libenet.a")
# shared on purpose: the C++ runtime's template instances and the like, identical in both
KEEP = re.compile(r"^(_ZN?K?St|_ZSt|_ZNKSt|_ZN9__gnu_cxx|_ZNK9__gnu_cxx|_ZT[VISTH]St|_ZT[VISTH]N9__gnu_cxx|_ZGVZ.*St|__|\.)")
# definitions not to pass on (MultiMelon sets its own)
DEFINES_SKIP = ("NDEBUG",)


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
    # what build.ninja says the program links: its objects and libraries, -l resolved to lib*.a
    text = open(os.path.join(build, "build.ninja"), encoding="utf-8", errors="replace").read()
    m = re.search(r"^build (?:[^:\s]*/)?(?:multimelon\.exe|MultiMelon\.app/Contents/MacOS/MultiMelon)[^:]*: \S+ (.*?)(?: \|.*)?$\n((?:  .*\n)*)",
                  text, re.M)
    if not m:
        sys.exit("no multimelon.exe or MultiMelon.app in " + build)
    files = m.group(1).split()
    libs = re.search(r"^  LINK_LIBRARIES = (.*)$", m.group(2), re.M)
    paths = re.search(r"^  LINK_PATH = (.*)$", m.group(2), re.M)
    search = [p[2:] for p in (paths.group(1).split() if paths else []) if p.startswith("-L")]
    if os.path.isdir("C:/msys64/ucrt64/lib"):
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


def azahar_libs(build):
    # the known libraries in order, then every other one the build made (with Vulkan: glslang, SPIRV-Tools,
    # sirit...); a group (or Apple's linker) takes them in any order
    known = [os.path.join(build, l) for l in AZAHAR_LIBS if os.path.exists(os.path.join(build, l))]
    names = {os.path.basename(l) for l in known}
    missing = [r for r in REQUIRED if r not in names]
    if missing:
        sys.exit("missing in %s: %s" % (build, ", ".join(missing)))
    seen = {os.path.normcase(os.path.abspath(l)) for l in known}
    extra = []
    for root, dirs, files in os.walk(build):
        dirs[:] = sorted(d for d in dirs if d != "CMakeFiles" and not SKIP.search(d))
        for f in sorted(files):
            p = os.path.join(root, f)
            if (f.endswith(".a") and not f.endswith(".dll.a") and not SKIP.search(f)
                    and os.path.normcase(os.path.abspath(p)) not in seen):
                extra.append(p)
    # (two libraries of the same name would overwrite each other's copy)
    for p in extra:
        if os.path.basename(p) in names:
            sys.exit("two libraries named " + os.path.basename(p))
        names.add(os.path.basename(p))
    return known + extra


def azahar_defines(build):
    # the definitions Azahar's core.cpp was compiled with (compile_commands.json: CMAKE_EXPORT_COMPILE_COMMANDS)
    path = os.path.join(build, "compile_commands.json")
    if not os.path.exists(path):
        sys.exit("no compile_commands.json in %s (configure Azahar with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)" % build)
    for e in json.load(open(path, encoding="utf-8")):
        if e["file"].replace("\\", "/").endswith("src/core/core.cpp"):
            args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
            out = []
            for i, t in enumerate(args):
                d = t[2:] if t.startswith("-D") and len(t) > 2 else (args[i + 1] if t == "-D" else None)
                if d and d.split("=")[0] not in DEFINES_SKIP and d not in out:
                    out.append(d)
            return out
    sys.exit("src/core/core.cpp not in " + path)


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
    a.add_argument("--ranlib", default="")
    a.add_argument("--macho", action="store_true")
    o = a.parse_args()
    os.makedirs(o.out, exist_ok=True)

    mine = set()
    for f in mm_inputs(o.mm_build):
        try:
            mine |= defined(o.nm, f)
        except subprocess.CalledProcessError:
            pass
    print("MultiMelon's link defines %d names" % len(mine))

    libs = azahar_libs(o.azahar_build)
    renames = {}
    for lib in libs:
        names = defined(o.nm, lib)
        whole = os.path.basename(lib) in RENAME_ALL
        for n in names:
            # (Mach-O: the C name is n[1:]; the new name keeps the "_" in front)
            plain = n[1:] if o.macho and n.startswith("_") else n
            if (whole or n in mine) and not KEEP.match(plain):
                renames[n] = ("_az_" + plain) if o.macho and n.startswith("_") else "az_" + n
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
        if o.ranlib:
            run([o.ranlib, dst])
        else:
            run([o.ar, "s", dst])
        order.append(dst.replace("\\", "/"))
    with open(os.path.join(o.out, "azahar-libs.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(order) + "\n")
    defines = azahar_defines(o.azahar_build)
    with open(os.path.join(o.out, "azahar-defines.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(defines) + "\n")
    print("wrote %d libraries to %s; Azahar's definitions: %s" % (len(order), o.out, " ".join(defines)))


if __name__ == "__main__":
    main()
