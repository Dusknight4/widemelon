// WideMelon renderer profile.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>

namespace WideMelon
{

// Immutable for the lifetime of a process: geometry and GL allocations must agree.
inline int Width()
{
    static const int width = []
    {
        const char* value = std::getenv("WIDEMELON_VIEW_WIDTH");
        if (!value)
            return 256;

        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end != value && *end == '\0' && parsed >= 256 && parsed <= 768 && parsed % 2 == 0)
            return static_cast<int>(parsed);
        return 256;
    }();
    return width;
}

inline bool Enabled()
{
    return Width() > 256;
}

// The battle scenery and info bar carried on beside the DS screen in the
// battle layout (GLRenderer::RenderWideBackdrop). Still being worked on, so it
// is off unless WIDEMELON_BATTLE_BACKDROP=1; that area is then left black.
inline bool BattleBackdropEnabled()
{
    static const bool enabled = []
    {
        const char* value = std::getenv("WIDEMELON_BATTLE_BACKDROP");
        return value && value[0] == '1';
    }();
    return enabled;
}

// Rounds down on both sides of the screen centre. Plain division rounds toward
// zero, so left of centre it rounded the other way, and things there moved
// unevenly against each other while the camera scrolled.
inline int32_t ProjectX(int32_t x)
{
    const int64_t scaled = static_cast<int64_t>(x) * 256;
    const int64_t width = Width();
    return static_cast<int32_t>(scaled >= 0 ? scaled / width : -((-scaled + width - 1) / width));
}

}
