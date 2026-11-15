#!/usr/bin/env python3
# Copyright 2026 WideMelon contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mk3gx.py PLUGIN.elf SETTINGS.plgInfo OUT.3gx [--discard-symbols]: a Luma3DS plugin file (3GX v2, "3GX$0002"),
# as 3gxtool v1.3 (gitlab.com/thepixellizeross/3gxtool) writes it, without its yaml-cpp/dynalo dependencies:
# the same header, strings, NOP-terminated default checksum payloads, 16-byte-aligned code/rodata/data, and
# symbol table. The .plgInfo is the usual subset of YAML: Key: value, the Version block, the Targets list.

import struct, sys

MAGIC = 0x3230303024584733
NOP = 0xE320F000
# 3gxtool's default checksum function (push {r6,r7,lr} ... pop {r6,r7,pc}; nop): payloads for both slots
DEFAULT_FUNC = bytes([0xC0, 0x40, 0x2D, 0xE9, 0x00, 0x70, 0xA0, 0xE3, 0x04, 0x60, 0x90, 0xE4, 0x06, 0x70, 0x87, 0xE0,
                      0x01, 0x00, 0x50, 0xE1, 0xFB, 0xFF, 0xFF, 0x1A, 0x07, 0x00, 0xA0, 0xE1, 0xC0, 0x80, 0xBD, 0xE8,
                      0x00, 0xF0, 0x20, 0xE3])
HEADER_SIZE = 16 + 72 + 40 + 8 + 12
SYM_DATA, SYM_FUNC, SYM_THUMB, SYM_ALTNAME = 1, 2, 4, 8


def parse_plginfo(text):
    out, key, block = {}, None, None
    for raw in text.splitlines():
        line = raw.split(" #")[0].rstrip() if not raw.lstrip().startswith("#") else ""
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        s = line.strip()
        if indent == 0:
            k, _, v = s.partition(":")
            key, v = k.strip(), v.strip()
            if v == "|":
                out[key], block = "", key
            elif v.startswith("[") and v.endswith("]"):
                out[key] = [x.strip() for x in v[1:-1].split(",") if x.strip()]
                block = None
            elif v:
                out[key], block = v.strip('"'), None
            else:
                out[key], block = None, key
        elif block is not None:
            if s.startswith("- "):
                out[block] = (out[block] or []) + [s[2:].strip()]
            elif isinstance(out.get(block), str) or out.get(block) == "":
                out[block] = (out[block] + "\n" if out[block] else "") + line[2:] if len(line) > 2 else out[block] + "\n"
            else:
                k, _, v = s.partition(":")
                d = out[block] if isinstance(out[block], dict) else {}
                d[k.strip()] = v.strip()
                out[block] = d
    return out


def read_elf(path):
    img = open(path, "rb").read()
    if img[:4] != b"\x7fELF":
        sys.exit("not an ELF")
    e_type, = struct.unpack_from("<H", img, 16)
    e_entry, e_phoff, e_shoff = struct.unpack_from("<III", img, 24)
    e_phnum, = struct.unpack_from("<H", img, 44)
    e_shnum, e_shstrndx = struct.unpack_from("<HH", img, 48)
    if e_type != 2:
        sys.exit("ELF must be executable")
    if e_phnum > 3:
        sys.exit("too many segments")
    segs, base, top = {}, None, None
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<8I", img, e_phoff + 32 * i)
        if not p_memsz:
            continue
        if base is None:
            base = p_vaddr
        elif p_vaddr != top:
            sys.exit("non-contiguous segments")
        if (p_memsz & 3) or (p_filesz & 3):
            sys.exit("segments must be word-aligned")
        if p_flags != 6 and p_memsz != p_filesz:
            sys.exit("only the data segment can have a BSS")
        kind = {5: "code", 4: "rodata", 6: "data"}.get(p_flags)
        if kind is None or kind in segs:
            sys.exit("invalid or repeated segment")
        segs[kind] = (img[p_offset:p_offset + p_filesz], p_memsz - p_filesz)
        top = p_vaddr + p_memsz
    if e_entry != base:
        sys.exit("the entry point must be the start of the code")
    if top - base >= 0x200000:
        print("WARNING: the executable is bigger than 2 MiB")

    # symbols, as 3gxtool picks them
    syms, names = [], bytearray()
    for i in range(e_shnum):
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link = struct.unpack_from("<7I", img, e_shoff + 40 * i)
        if sh_type != 2:    # SHT_SYMTAB
            continue
        str_off = struct.unpack_from("<I", img, e_shoff + 40 * sh_link + 16)[0]
        name = lambda o: img[str_off + o:img.index(b"\0", str_off + o)]
        entries = []
        for j in range(sh_size // 16):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", img, sh_offset + 16 * j)
            if (st_info & 0xF) in (3, 4):   # STT_SECTION, STT_FILE
                continue
            entries.append((st_value, st_size, st_info, st_other, name(st_name)))
        entries.sort(key=lambda s: (s[0], 0 if s[4].startswith(b"$") else 1))
        dedup = []
        for s in entries:
            if dedup and dedup[-1] == s:
                continue
            dedup.append(s)
        kind, last = 0, 0
        for k, s in enumerate(dedup):
            nm = s[4]
            if nm[:1] == b"$" and len(nm) == 2:
                if k + 1 < len(dedup) and s[0] == dedup[k + 1][0]:
                    kind = {b"a": SYM_FUNC, b"p": SYM_FUNC, b"b": SYM_FUNC | SYM_THUMB, b"t": SYM_FUNC | SYM_THUMB,
                            b"d": SYM_DATA}.get(nm[1:2], kind)
                continue
            flags = kind | (SYM_ALTNAME if last == s[0] else 0)
            syms.append(struct.pack("<IHHI", s[0], s[1] & 0xFFFF, flags, len(names)))
            names += nm + b"\0"
            last, kind = s[0], 0
    if not syms:
        sys.exit("ELF has no symbol table")
    return segs, syms, names


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    discard = "--discard-symbols" in sys.argv
    elf, settings_path, out_path = args
    segs, syms, names = read_elf(elf)
    st = parse_plginfo(open(settings_path, encoding="utf-8").read())

    v = st.get("Version") or {}
    version = (int(v.get("Major", 0), 0) << 24) | (int(v.get("Minor", 0), 0) << 16) | (int(v.get("Revision", 0), 0) << 8)
    compat = {"console": 0, "citra": 1, "any": 2}.get((st.get("Compatibility") or "any").lower(), 2)
    memsize = {"5mib": 0, "2mib": 1, "10mib": 2}.get((st.get("MemorySize") or "5mib").lower(), 0)
    truthy = lambda k: (st.get(k) or "").lower() == "true"
    flags = 1 | 2 | (memsize << 2) | (compat << 4) | (truthy("EventsSelfManaged") << 6) | (truthy("SwapNotNeeded") << 7) \
        | (truthy("UsePrivateMemory") << 8) | (truthy("AllowHomebrewLoad") << 9)
    targets = [int(x, 0) for x in (st.get("Targets") or [])]
    if targets and targets[0] == 0:
        targets = []

    code, _ = segs.get("code", (b"", 0))
    rodata, _ = segs.get("rodata", (b"", 0))
    data, bss = segs.get("data", (b"", 0))
    blob = code + rodata + data
    checksum = sum(struct.unpack("<%dI" % (len(blob) // 4), blob)) & 0xFFFFFFFF

    out = bytearray(HEADER_SIZE)
    msgs = {}
    for key in ("Title", "Author", "Summary", "Description"):
        text = st.get(key)
        if text:
            msgs[key] = (len(out), len(text.encode()) + 1)
            out += text.encode() + b"\0"
    tgt_off = len(out) if targets else 0
    for t in targets:
        out += struct.pack("<I", t)
    exe_dec = len(out)
    out += DEFAULT_FUNC
    swap_enc = len(out)
    out += DEFAULT_FUNC
    swap_dec = len(out)
    out += DEFAULT_FUNC
    out += b"\0" * (16 - (len(out) & 0xF))      # (as 3gxtool: 1-16 bytes, never 0)
    code_off = len(out)
    out += code
    ro_off = len(out)
    out += rodata
    data_off = len(out)
    out += data
    if discard:
        sym_count = sym_off = name_off = 0
    else:
        sym_count, sym_off = len(syms), len(out)
        out += b"".join(syms)
        name_off = len(out)
        out += names

    m = lambda k: msgs.get(k, (0, 0))
    infos = struct.pack("<8I", m("Author")[1], m("Author")[0], m("Title")[1], m("Title")[0],
                        m("Summary")[1], m("Summary")[0], m("Description")[1], m("Description")[0])
    infos += struct.pack("<II", flags, checksum) + b"\0" * 32
    exe = struct.pack("<10I", code_off, ro_off, data_off, len(code), len(rodata), len(data), bss, exe_dec, swap_enc, swap_dec)
    header = struct.pack("<QII", MAGIC, version, 0) + infos + exe + struct.pack("<II", len(targets), tgt_off) \
        + struct.pack("<III", sym_count, sym_off, name_off)
    assert len(header) == HEADER_SIZE
    out[:HEADER_SIZE] = header
    open(out_path, "wb").write(out)
    print("%s: %d bytes (code %d, rodata %d, data %d + bss %d), %d symbols" % (out_path, len(out), len(code), len(rodata),
          len(data), bss, sym_count))


if __name__ == "__main__":
    main()
