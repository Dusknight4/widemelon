// WideMelon: tells which part of a Pokémon game is running (a battle, the PC's boxes, the bag, the menu).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <iterator>
#include <vector>

#include "types.h"

namespace WideMelon
{

// DS games load the code for each part of the game (overworld, battles, menus)
// as ARM9 overlays at fixed RAM addresses. Pokémon Platinum keeps its battle
// engine in overlay 16, which is loaded while the screen is black between the
// encounter effect and the battle's intro, and stays loaded until the
// overworld code is loaded back over it after the battle. So a battle is
// running while the start of overlay 16, as stored on the game card, is in RAM.
// HeartGold and SoulSilver: the battle is overlay 12, the PC's boxes overlay 14,
// the bag overlay 15 (each loaded over the overworld's code, while the screen
// is black); their overlays are compressed on the card, so the start of each is
// unpacked here. Their menu (X) is open while the field system's byte +0xD2 isn't
// 0 in its low 6 bits (start_menu.c's unkD2_0; HeartGold 10/22 has 2 there while
// the menu's cursor is up, 0 once it is closed), the field system being the one
// sFieldSysPtr points to (found through sub_0203E324, which reads it). The menu
// draws a bar over the top screen's bottom fifth, on the main engine's BG3
// (start_menu.c: Task_StartMenu_DrawCursor). The PC: the script running is
// std_pokecenter_pc (2010), its ScriptEnvironment being the environment of the field
// system's task (or of a task it called). The party screen (Pokémon) and a Pokémon's
// summary are in the ARM9 binary, not overlays: one is running while the field system's
// process manager has no parent app (the field's is gone) and its child app is
// gOverlayTemplate_PartyMenu's or gOverlayTemplate_PokemonSummary's (FieldProcessManager:
// parent +0, child +4; an OverlayManager starts with its template, the init function
// first). (A summary opened from the PC's boxes runs with their overlay still loaded.)
class BattleDetector
{
public:
    enum Mode { Normal = 0, Battle, Boxes, Bag, Menu, PC, Party, Summary };

    // Finds the overlays of a supported game; otherwise Detect() stays Normal.
    void Init(const melonDS::u8* rom, melonDS::u32 romSize)
    {
        Count = 0;
        FieldSysPtr = 0;
        PartyInit = SummaryInit = 0;
        if (!rom || romSize < 0x200) return;
        Rom = rom;
        RomSize = romSize;

        const bool pt = std::memcmp(rom + 0x0C, "CPU", 3) == 0;
        const bool hgss = std::memcmp(rom + 0x0C, "IPK", 3) == 0 || std::memcmp(rom + 0x0C, "IPG", 3) == 0;
        if (pt)
            AddOverlay(16, Battle);
        else if (hgss)
        {
            AddOverlay(12, Battle);
            AddOverlay(14, Boxes);
            AddOverlay(15, Bag);
            FindFieldSystem();
        }
    }

    bool IsSupported() const { return Count > 0; }

    Mode Detect(const melonDS::u8* mainRAM, melonDS::u32 ramMask) const
    {
        if (!mainRAM) return Normal;
        for (int i = 0; i < Count; i++)
        {
            const Part& p = Parts[i];
            if (p.RAMOffset + sizeof(p.Signature) <= ramMask + 1
                && std::memcmp(mainRAM + p.RAMOffset, p.Signature, sizeof(p.Signature)) == 0)
                return p.mode;
        }
        if (FieldSysPtr)
        {
            const melonDS::u32 fs = Read32(mainRAM, ramMask, FieldSysPtr);
            if ((PartyInit || SummaryInit) && (fs >> 24) == 0x02)
            {
                const melonDS::u32 pm = Read32(mainRAM, ramMask, fs);
                if ((pm >> 24) == 0x02 && Read32(mainRAM, ramMask, pm) == 0)
                {
                    const melonDS::u32 child = Read32(mainRAM, ramMask, pm + 4);
                    const melonDS::u32 init = (child >> 24) == 0x02 ? Read32(mainRAM, ramMask, child) : 0;
                    if (init && init == PartyInit) return Party;
                    if (init && init == SummaryInit) return Summary;
                }
            }
            if ((fs >> 24) == 0x02 && (mainRAM[(fs + 0xD2) & ramMask] & 0x3F) != 0)
                return Menu;
            // (FieldSystem.taskman +0x10; TaskManager: prev +0, env +0xC; ScriptEnvironment: check +0,
            // activeScriptNumber +0xA)
            melonDS::u32 task = (fs >> 24) == 0x02 ? Read32(mainRAM, ramMask, fs + 0x10) : 0;
            for (int depth = 0; depth < 4 && (task >> 24) == 0x02; depth++)
            {
                const melonDS::u32 env = Read32(mainRAM, ramMask, task + 0xC);
                if ((env >> 24) == 0x02 && Read32(mainRAM, ramMask, env) == 222271
                    && (Read32(mainRAM, ramMask, env + 8) >> 16) == 2010)
                    return PC;
                task = Read32(mainRAM, ramMask, task);
            }
        }
        return Normal;
    }

    bool InBattle(const melonDS::u8* mainRAM, melonDS::u32 ramMask) const { return Detect(mainRAM, ramMask) == Battle; }

private:
    struct Part
    {
        Mode mode;
        melonDS::u32 RAMOffset;
        melonDS::u8 Signature[64];
    };

    static melonDS::u32 Read32(const melonDS::u8* p, melonDS::u32 mask, melonDS::u32 addr)
    {
        const melonDS::u32 o = addr & mask;
        if (o + 4 > mask + 1) return 0;
        return p[o] | p[o + 1] << 8 | p[o + 2] << 16 | (melonDS::u32)p[o + 3] << 24;
    }

    melonDS::u32 RomRead32(melonDS::u32 offset) const
    {
        if (offset > RomSize - 4) return 0;
        return Rom[offset] | Rom[offset + 1] << 8 | Rom[offset + 2] << 16 | (melonDS::u32)Rom[offset + 3] << 24;
    }

    // MIi_UncompressBackward on buf[0, end) (as FS_LoadOverlay unpacks a compressed overlay)
    static bool UncompressBackward(std::vector<melonDS::u8>& buf, melonDS::u32 end)
    {
        auto rd = [&](melonDS::u32 o) { return buf[o] | buf[o + 1] << 8 | buf[o + 2] << 16 | (melonDS::u32)buf[o + 3] << 24; };
        if (end < 8 || end > buf.size()) return false;
        const melonDS::u32 word = rd(end - 8), extra = rd(end - 4);
        const melonDS::u32 complen = word & 0xFFFFFF, hdrlen = word >> 24;
        if (complen > end || hdrlen < 8 || hdrlen > complen || extra > 0x400000) return false;
        buf.resize(end + extra);
        melonDS::u32 src = end - hdrlen, dst = end + extra;
        const melonDS::u32 lo = end - complen;
        while (src > lo)
        {
            melonDS::u8 ctrl = buf[--src];
            for (int i = 0; i < 8 && src > lo; i++, ctrl <<= 1)
            {
                if (ctrl & 0x80)
                {
                    if (src < lo + 2) return false;
                    const melonDS::u8 b1 = buf[src - 1], b2 = buf[src - 2];
                    src -= 2;
                    const melonDS::u32 disp = (((b1 & 0xF) << 8) | b2) + 3;
                    for (int n = (b1 >> 4) + 3; n > 0; n--)
                    {
                        if (dst == 0 || dst + disp - 1 >= buf.size()) return false;
                        dst--;
                        buf[dst] = buf[dst + disp];
                    }
                }
                else
                {
                    if (dst == 0) return false;
                    dst--;
                    buf[dst] = buf[--src];
                }
            }
        }
        return true;
    }

    // the start of an overlay as it is in RAM once loaded, unpacked if need be
    void AddOverlay(melonDS::u32 id, Mode mode)
    {
        if (Count >= MaxParts) return;
        const melonDS::u32 fat = RomRead32(0x48), table = RomRead32(0x50), tableSize = RomRead32(0x54);
        for (melonDS::u32 entry = table; entry + 32 <= table + tableSize && entry + 32 <= RomSize; entry += 32)
        {
            if (RomRead32(entry) != id) continue;

            const melonDS::u32 ram = RomRead32(entry + 4), size = RomRead32(entry + 8);
            const melonDS::u32 file = RomRead32(entry + 24), flags = RomRead32(entry + 28);
            const melonDS::u32 start = RomRead32(fat + file * 8), end = RomRead32(fat + file * 8 + 4);
            Part& p = Parts[Count];
            if (size < sizeof(p.Signature) || ram < 0x02000000 || ram + sizeof(p.Signature) > 0x02400000) return;
            if (end <= start || end > RomSize) return;
            if ((flags >> 24) & 1)
            {
                std::vector<melonDS::u8> buf(Rom + start, Rom + end);
                if (!UncompressBackward(buf, flags & 0xFFFFFF) || buf.size() < sizeof(p.Signature)) return;
                std::memcpy(p.Signature, buf.data(), sizeof(p.Signature));
            }
            else
            {
                if (end - start < sizeof(p.Signature)) return;
                std::memcpy(p.Signature, Rom + start, sizeof(p.Signature));
            }
            bool blank = true;
            for (melonDS::u8 b : p.Signature) blank = blank && b == 0;
            if (blank) return;

            p.mode = mode;
            p.RAMOffset = ram - 0x02000000;
            Count++;
            return;
        }
    }

    // HeartGold/SoulSilver: sFieldSysPtr, from sub_0203E324 ("ldr r0, =sFieldSysPtr; ldr r0, [r0]; ldr r0, [r0, #4];
    // cmp r0, #0; bne; movs r0, #0; bx lr; ldr r0, [r0, #0x14]; bx lr") in the unpacked ARM9 binary
    void FindFieldSystem()
    {
        const melonDS::u32 off = RomRead32(0x20), ram = RomRead32(0x28), size = RomRead32(0x2C);
        if (ram != 0x02000000 || size == 0 || size > 0x3F0000 || off + size + 8 > RomSize) return;
        std::vector<melonDS::u8> arm9(Rom + off, Rom + off + size);
        if (RomRead32(off + size) == 0xDEC00621)
        {
            const melonDS::u32 params = RomRead32(off + size + 4);
            if (params + 0x18 > size) return;
            const melonDS::u32 cend = arm9[params + 0x14] | arm9[params + 0x15] << 8 | arm9[params + 0x16] << 16 | (melonDS::u32)arm9[params + 0x17] << 24;
            if (cend && (cend <= ram || !UncompressBackward(arm9, cend - ram))) return;
        }
        static const melonDS::u8 pat[18] = {0, 0x48, 0x00, 0x68, 0x40, 0x68, 0x00, 0x28, 0x01, 0xD1, 0x00, 0x20, 0x70, 0x47, 0x40, 0x69, 0x70, 0x47};
        melonDS::u32 found = 0, matches = 0;
        for (melonDS::u32 s = 0; s + sizeof(pat) <= arm9.size(); s += 2)
        {
            bool ok = true;
            for (melonDS::u32 i = 1; i < sizeof(pat) && ok; i++) ok = arm9[s + i] == pat[i];
            if (ok) { found = s; matches++; }
        }
        if (matches != 1) return;
        const melonDS::u32 lit = ((ram + found + 4) & ~3u) + arm9[found] * 4 - ram;
        if (lit + 4 > arm9.size()) return;
        const melonDS::u32 ptr = arm9[lit] | arm9[lit + 1] << 8 | arm9[lit + 2] << 16 | (melonDS::u32)arm9[lit + 3] << 24;
        if ((ptr >> 24) == 0x02) FieldSysPtr = ptr;
        // gOverlayTemplate_PartyMenu. init: push {r4, r5, lr}; sub sp, #0xC; adds r5, r0, #0; movs r0, #0;
        // adds r1, r0, #0; four calls; movs r0, #1; lsls r0, r0, #26; ldr r2, [r0]. main: push {r3-r5, lr};
        // adds r4, r1, #0; a call; ldr r1, [r4]; adds r5, r0, #0; cmp r1, #0x21 (its 34 states)
        static const short partyInit[] = {0x30, 0xB5, 0x83, 0xB0, 0x05, 0x1C, 0x00, 0x20, 0x01, 0x1C,
                                          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                          0x01, 0x20, 0x80, 0x06, 0x02, 0x68};
        static const short partyMain[] = {0x38, 0xB5, 0x0C, 0x1C, -1, -1, -1, -1, 0x21, 0x68, 0x05, 0x1C, 0x21, 0x29};
        PartyInit = FindApp(arm9, ram, partyInit, std::size(partyInit), partyMain, std::size(partyMain));
        // gOverlayTemplate_PokemonSummary. init: push {r3-r7, lr}; adds r6, r0, #0; movs r0, #0; adds r1, r0, #0;
        // four calls; movs r1, #1; lsls r1, r1, #26; ldr r0, [r1]. main: as the party's, with cmp r1, #0x16; bls
        static const short summaryInit[] = {0xF8, 0xB5, 0x06, 0x1C, 0x00, 0x20, 0x01, 0x1C,
                                            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                            0x01, 0x21, 0x89, 0x06, 0x08, 0x68};
        static const short summaryMain[] = {0x38, 0xB5, 0x0C, 0x1C, -1, -1, -1, -1, 0x21, 0x68, 0x05, 0x1C, 0x16, 0x29, 0x00, 0xD9};
        SummaryInit = FindApp(arm9, ram, summaryInit, std::size(summaryInit), summaryMain, std::size(summaryMain));
    }

    // an app's init function (with its Thumb bit): the one template {init, main, exit, FS_OVERLAY_ID_NONE}
    // whose init and main start as given (-1: any byte, for the calls, which depend on the build); 0 if
    // none or more than one
    static melonDS::u32 FindApp(const std::vector<melonDS::u8>& arm9, melonDS::u32 ram,
                                const short* initPat, size_t initLen, const short* mainPat, size_t mainLen)
    {
        auto rd = [&](melonDS::u32 o) { return arm9[o] | arm9[o + 1] << 8 | arm9[o + 2] << 16 | (melonDS::u32)arm9[o + 3] << 24; };
        auto codeAt = [&](melonDS::u32 fn, const short* pat, size_t n) {
            if (!(fn & 1) || fn - 1 < ram || fn - 1 + n > ram + arm9.size()) return false;
            const melonDS::u32 o = fn - 1 - ram;
            for (size_t i = 0; i < n; i++)
                if (pat[i] >= 0 && arm9[o + i] != pat[i]) return false;
            return true;
        };
        melonDS::u32 found = 0, matches = 0;
        for (melonDS::u32 o = 0; o + 16 <= arm9.size(); o += 4)
        {
            if (rd(o + 12) != 0xFFFFFFFF) continue;
            const melonDS::u32 init = rd(o), main = rd(o + 4);
            if (codeAt(init, initPat, initLen) && codeAt(main, mainPat, mainLen))
            {
                found = init;
                matches++;
            }
        }
        return matches == 1 ? found : 0;
    }

    static constexpr int MaxParts = 4;
    Part Parts[MaxParts] {};
    int Count = 0;
    melonDS::u32 FieldSysPtr = 0;
    melonDS::u32 PartyInit = 0;     // PartyMenuApp_Init (with its Thumb bit)
    melonDS::u32 SummaryInit = 0;   // the summary's init
    const melonDS::u8* Rom = nullptr;
    melonDS::u32 RomSize = 0;
};

}
