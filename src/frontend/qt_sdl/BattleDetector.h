// WideMelon: tells when a Pokémon game is in a battle.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>

#include "types.h"

namespace WideMelon
{

// DS games load the code for each part of the game (overworld, battles, menus)
// as ARM9 overlays at fixed RAM addresses. Pokémon Platinum keeps its battle
// engine in overlay 16, which is loaded while the screen is black between the
// encounter effect and the battle's intro, and stays loaded until the
// overworld code is loaded back over it after the battle. So a battle is
// running while the start of overlay 16, as stored on the game card, is in RAM.
class BattleDetector
{
public:
    // Finds the battle overlay of a supported game; otherwise InBattle() stays false.
    void Init(const melonDS::u8* rom, melonDS::u32 romSize)
    {
        Supported = false;
        if (!rom || romSize < 0x200) return;

        // games by the first three letters of their game code (the fourth is the region)
        struct Game { char code[4]; melonDS::u32 overlay; };
        static const Game games[] = {
            {"CPU", 16}, // Pokémon Platinum
        };
        const Game* game = nullptr;
        for (const Game& g : games)
            if (std::memcmp(rom + 0x0C, g.code, 3) == 0)
                game = &g;
        if (!game) return;

        auto read32 = [&](melonDS::u32 offset) -> melonDS::u32
        {
            if (offset > romSize - 4) return 0;
            return rom[offset] | rom[offset+1] << 8 | rom[offset+2] << 16 | (melonDS::u32)rom[offset+3] << 24;
        };
        const melonDS::u32 fat = read32(0x48), table = read32(0x50), tableSize = read32(0x54);
        for (melonDS::u32 entry = table; entry + 32 <= table + tableSize && entry + 32 <= romSize; entry += 32)
        {
            if (read32(entry) != game->overlay) continue;

            const melonDS::u32 ram = read32(entry + 4), size = read32(entry + 8);
            const melonDS::u32 file = read32(entry + 24), flags = read32(entry + 28);
            const melonDS::u32 start = read32(fat + file * 8);
            if ((flags >> 24) & 1) return;              // compressed: RAM holds something else
            if (size < sizeof(Signature) || ram < 0x02000000 || ram + sizeof(Signature) > 0x02400000) return;
            if (start > romSize - sizeof(Signature)) return;

            std::memcpy(Signature, rom + start, sizeof(Signature));
            bool blank = true;
            for (melonDS::u8 b : Signature) blank = blank && b == 0;
            if (blank) return;

            RAMOffset = ram - 0x02000000;
            Supported = true;
            return;
        }
    }

    bool IsSupported() const { return Supported; }

    bool InBattle(const melonDS::u8* mainRAM, melonDS::u32 ramMask) const
    {
        if (!Supported || !mainRAM || RAMOffset + sizeof(Signature) > ramMask + 1) return false;
        return std::memcmp(mainRAM + RAMOffset, Signature, sizeof(Signature)) == 0;
    }

private:
    bool Supported = false;
    melonDS::u32 RAMOffset = 0;
    melonDS::u8 Signature[64] = {};
};

}
