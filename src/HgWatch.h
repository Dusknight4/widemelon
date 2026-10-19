/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

// WideMelon: watches Pokemon HeartGold/SoulSilver's link layer from outside (a test aid, with
// WIDEMELON_PXI_RING set): every change of what decides whether the game gives up on a link,
// with the frame number, to see which of its checks fires. The structures are found by their
// shape in main RAM (a modded game moves things), laid out as in the decompiled HG source
// (comm_state.c, comm_wireless.c, communication_core.c), checked against a running game:
//   CommStateWork (gCommState): MATH_Rand32 context at +0x10 (mul 0x5D588B656C078965 at +0x18,
//     add 0x269EC3 at +0x20), stateFunc +0x28, timer +0x48, memberCount +0x4F, linkMode +0x52,
//     watchMembers +0x57, reportErrors +0x58, errorScreen +0x5A, errorKind +0x61
//   CommWirelessWork (0xD98 bytes): parentTimers[16] +0xD44 (scanned players, frames left),
//     profile +0xD78, ruleset +0xD7C, ggid +0xD80, beaconBuffer +0xD84, beacon +0xD88 (rounded up
//     to 32), firstCount +0xD8E, shutdown +0xD92, flags +0xD95 (bit 0 linkLost, 1 detectDrops,
//     2 lostWhenAlone)
//   gCommCore.work: command rings at +0x51C (12 bytes each, carved from the buffer at +0x48C),
//     players bitmap +0x694, link error flag +0x6B8, backlog counters +0x668 (child), +0x66C (per player)
#ifndef HGWATCH_H
#define HGWATCH_H

#include <cstdio>
#include <cstring>
#include <string>

struct HgWatch
{
    const unsigned char* Ram = nullptr;
    unsigned RamMask = 0;
    unsigned FrameNo = 0;
    void (*Out)(unsigned frame, const char* text) = nullptr;

    unsigned CommState = 0, Wireless = 0, Core = 0;
    std::string Last[3];

    unsigned R32(unsigned a) const { unsigned v; std::memcpy(&v, Ram + (a & RamMask & ~3u), 4); return v; }
    unsigned R16(unsigned a) const { return R32(a) >> ((a & 2) * 8) & 0xFFFF; }
    unsigned R8(unsigned a) const { return Ram[a & RamMask]; }
    static bool InRam(unsigned p) { return p >= 0x02000000 && p < 0x02400000; }

    bool IsCommState(unsigned a) const
    {
        return R32(a + 0x18) == 0x6C078965 && R32(a + 0x1C) == 0x5D588B65 && R32(a + 0x20) == 0x00269EC3 && R32(a + 0x24) == 0
            && InRam(R32(a + 0x28)) && R8(a + 0x52) < 41 && R8(a + 0x57) < 2 && R8(a + 0x58) < 2;
    }
    bool IsWireless(unsigned a) const
    {
        const unsigned bb = R32(a + 0xD84), b = R32(a + 0xD88);
        if (!InRam(bb) || b != ((bb + 31) & ~31u) || !InRam(R32(a + 0xD78)) || !InRam(R32(a + 0xD7C)) || R32(a + 0xD80) == 0) return false;
        for (int i = 0; i < 16; i++) if (R16(a + 0xD44 + i * 2) > 300) return false;
        return R8(a + 0xD92) <= 3;
    }
    bool IsCore(unsigned a) const
    {
        const unsigned base = R32(a + 0x48C);
        if (!InRam(base)) return false;
        for (int k = 0; k < 8; k++)
        {
            const unsigned p = R32(a + 0x51C + 12 * k);
            if (!InRam(R32(a + 0x4B0 + 12 * k)) || p < base || p >= base + 0x10000) return false;
        }
        for (unsigned o = 0x668; o <= 0x688; o += 4) { const int v = (int)R32(a + o); if (v < -1000 || v > 1000) return false; }
        return R8(a + 0x6B8) < 2 && R16(a + 0x694) < 0x100 && R8(a + 0x6B0) < 2;
    }
    bool Linked() const { return CommState && R8(CommState + 0x57) == 1; }

    template <class... A> void Say(const char* f, A... a)
    {
        char b[512];
        std::snprintf(b, sizeof b, f, a...);
        if (Out) Out(FrameNo, b);
    }
    // a slice of main RAM a frame (all of it in 8 frames), so looking never costs a frame
    unsigned ScanAt = 0x02000000;
    void Scan()
    {
        const unsigned end = ScanAt + 0x80000;
        for (unsigned a = ScanAt; a < end && a < 0x02400000 - 0xE00; a += 4)
        {
            if (!(CommState && IsCommState(CommState)) && IsCommState(a)) { CommState = a; Say("found CommStateWork at %08X", a); }
            if (!(Wireless && IsWireless(Wireless)) && IsWireless(a)) { Wireless = a; Say("found CommWirelessWork at %08X", a); }
            if (Linked() && !(Core && IsCore(Core)) && IsCore(a)) { Core = a; Say("found gCommCore.work at %08X", a); }
        }
        ScanAt = end >= 0x02400000 - 0xE00 ? 0x02000000 : end;
    }

    // once a frame
    void Frame(const unsigned char* ram, unsigned ramMask, unsigned frame)
    {
        Ram = ram; RamMask = ramMask; FrameNo = frame;
        if (CommState && !IsCommState(CommState)) { Say("CommStateWork gone"); CommState = 0; Last[0].clear(); }
        if (Wireless && !IsWireless(Wireless)) { Say("CommWirelessWork gone"); Wireless = 0; Last[1].clear(); }
        if (Core && !IsCore(Core)) { Say("gCommCore.work gone"); Core = 0; Last[2].clear(); }
        // (CommWirelessWork is allocated right after CommStateWork: looked at there first)
        if (CommState && !Wireless && IsWireless(CommState + 0xC4)) { Wireless = CommState + 0xC4; Say("found CommWirelessWork at %08X", Wireless); }
        if (!CommState || !Wireless || (Linked() && !Core)) Scan();
        char b[400];
        if (CommState)
        {
            const unsigned a = CommState;
            std::snprintf(b, sizeof b, "state %08X linkMode %u watch %u report %u errScreen %u errKind %u members %u", R32(a + 0x28), R8(a + 0x52),
                          R8(a + 0x57), R8(a + 0x58), R8(a + 0x5A), R8(a + 0x61), R8(a + 0x4F));
            if (Last[0] != b) { Say("comm %s (timer %d)", b, (int)R32(a + 0x48)); Last[0] = b; }
        }
        if (Wireless)
        {
            const unsigned a = Wireless, f = R8(a + 0xD95);
            std::string t, key;
            for (int i = 0; i < 16; i++)
            {
                const unsigned v = R16(a + 0xD44 + i * 2);
                if (!v) continue;
                char e[24];
                std::snprintf(e, sizeof e, " %d:%u", i, v);
                t += e;
                std::snprintf(e, sizeof e, " %d:%s", i, v > 290 ? "F" : v > 60 ? "o" : "L");     // (printed when one of these changes)
                key += e;
            }
            std::snprintf(b, sizeof b, "linkLost %u detectDrops %u lostWhenAlone %u shutdown %u firstCount %04X parents[%s ]",
                          f & 1, f >> 1 & 1, f >> 2 & 1, R8(a + 0xD92), R16(a + 0xD8E), key.c_str());
            if (Last[1] != b) { Say("wireless linkLost %u detectDrops %u lostWhenAlone %u shutdown %u firstCount %04X parents (frames left)[%s ]",
                                    f & 1, f >> 1 & 1, f >> 2 & 1, R8(a + 0xD92), R16(a + 0xD8E), t.c_str()); Last[1] = b; }
        }
        if (Core)
        {
            const unsigned a = Core;
            std::snprintf(b, sizeof b, "players %04X linkError %u", R16(a + 0x694), R8(a + 0x6B8));
            if (Last[2] != b) { Say("core %s (backlog child %d, players %d,%d)", b, (int)R32(a + 0x668), (int)R32(a + 0x66C), (int)R32(a + 0x670)); Last[2] = b; }
        }
    }
};

#endif
