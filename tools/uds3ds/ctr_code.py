#!/usr/bin/env python3
# Copyright 2026 WideMelon contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ctr_code.py FILE OUT: the code (ExeFS ".code", unpacked) of a decrypted 3DS title, from a .cia (its first
# content) or a .cxi/.3ds/.cci (its first partition), read in place (no copy of the whole file).

import struct, sys


def blz_decompress(data: bytes) -> bytes:
    # backward LZ, as the 3DS's loader (and the DS's MIi_UncompressBackward) unpacks code
    buf = bytearray(data)
    end = len(buf)
    word, extra = struct.unpack_from("<II", buf, end - 8)
    complen, hdrlen = word & 0xFFFFFF, word >> 24
    buf.extend(b"\0" * extra)
    src, dst, lo = end - hdrlen, end + extra, end - complen
    while src > lo:
        src -= 1
        ctrl = buf[src]
        for _ in range(8):
            if src <= lo:
                break
            if ctrl & 0x80:
                src -= 2
                b1, b2 = buf[src + 1], buf[src]
                disp = (((b1 & 0xF) << 8) | b2) + 3
                for _ in range((b1 >> 4) + 3):
                    dst -= 1
                    buf[dst] = buf[dst + disp]
            else:
                src -= 1
                dst -= 1
                buf[dst] = buf[src]
            ctrl = (ctrl << 1) & 0xFF
    return bytes(buf)


def ncch_offset(f):
    f.seek(0)
    head = f.read(0x200)
    if head[0x100:0x104] == b"NCCH":
        return 0
    if head[0x100:0x104] == b"NCSD":
        return struct.unpack_from("<I", head, 0x120)[0] * 0x200    # partition 0
    # a CIA: header, certs, ticket, TMD, then the contents, each 64-aligned
    hdr, _, _, cert, tik, tmd = struct.unpack_from("<IHHIII", head, 0)
    a = lambda x: (x + 63) & ~63
    return a(a(a(a(hdr) + cert) + tik) + tmd)


def main():
    path, out = sys.argv[1], sys.argv[2]
    with open(path, "rb") as f:
        base = ncch_offset(f)
        f.seek(base)
        ncch = f.read(0x200)
        assert ncch[0x100:0x104] == b"NCCH", "no NCCH"
        if not (ncch[0x18F] & 4):
            sys.exit("this title is encrypted (NoCrypto flag not set)")
        exefs = base + struct.unpack_from("<I", ncch, 0x1A0)[0] * 0x200
        exh_flags = None
        f.seek(base + 0x200)
        exheader = f.read(0x400)
        compressed = bool(exheader[0xD] & 1)
        f.seek(exefs)
        hdr = f.read(0x200)
        for i in range(10):
            name = hdr[i * 16:i * 16 + 8].rstrip(b"\0")
            off, size = struct.unpack_from("<II", hdr, i * 16 + 8)
            if name == b".code":
                f.seek(exefs + 0x200 + off)
                code = f.read(size)
                if compressed:
                    code = blz_decompress(code)
                text_addr = struct.unpack_from("<I", exheader, 0x10)[0]
                open(out, "wb").write(code)
                print("code: %d bytes (%s), .text at 0x%08X, title %s" % (len(code), "unpacked" if compressed else "plain",
                      text_addr, exheader[:8].rstrip(b"\0").decode(errors="replace")))
                return
    sys.exit("no .code")


if __name__ == "__main__":
    main()
