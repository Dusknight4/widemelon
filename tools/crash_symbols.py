#!/usr/bin/env python3
# Copyright 2026 WideMelon contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# crash_symbols.py UNSTRIPPED_EXE CRASH_TXT [--nm nm]: a crash report (crashes/crash-*.txt, see
# src/frontend/qt_sdl/CrashHandler.h) with each "<exe>+0xOFFSET" turned into the function it falls in,
# from an unstripped copy of the very same build (the release exe is stripped: same code, no names).

import bisect, re, subprocess, sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    nm = "nm"
    if "--nm" in sys.argv:
        nm = sys.argv[sys.argv.index("--nm") + 1]
        args = [a for a in args if a != nm]
    exe, report = args[0], args[1]

    base = None
    for line in subprocess.run(["objdump", "-p", exe], capture_output=True, text=True).stdout.splitlines():
        if line.startswith("ImageBase"):
            base = int(line.split()[1], 16)
    syms = []
    out = subprocess.run([nm, "-C", "--defined-only", exe], capture_output=True, text=True, errors="replace").stdout
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    addrs = [s[0] for s in syms]

    for line in open(report, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        m = re.search(r"(multimelon[^+\s]*\.exe)\+0x([0-9a-fA-F]+)", line)
        if m and base is not None:
            a = base + int(m.group(2), 16)
            i = bisect.bisect_right(addrs, a) - 1
            if i >= 0:
                line += "   %s+0x%x" % (syms[i][1], a - syms[i][0])
        print(line)


if __name__ == "__main__":
    main()
