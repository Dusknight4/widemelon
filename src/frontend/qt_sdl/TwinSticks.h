// WideMelon: either analog stick works for a stick binding.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

#include <SDL2/SDL.h>

namespace WideMelon
{

// A joystick binding can include one stick direction (an axis, + or -). With
// twin sticks, the same direction on the other analog stick counts too, so on
// a Switch Pro controller a binding to Axis 1 also works with Axis 3, and
// Axis 2 with Axis 4. Which axes belong to which stick comes from the
// controller's SDL mapping, so generic controllers whose axes are laid out
// differently (with triggers among the first four, say) aren't affected.

// Pairs each stick's X and Y axis with the other stick's: twin[axis] is the
// matching axis on the other stick, or -1.
inline void FindStickTwins(SDL_GameController* controller, int twin[16])
{
    for (int i = 0; i < 16; i++) twin[i] = -1;
    if (!controller) return;

    auto stickAxis = [controller](SDL_GameControllerAxis axis)
    {
        SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForAxis(controller, axis);
        if (bind.bindType != SDL_CONTROLLER_BINDTYPE_AXIS || bind.value.axis < 0 || bind.value.axis >= 16)
            return -1;
        return bind.value.axis;
    };
    const int pairs[2][2] = {
        {stickAxis(SDL_CONTROLLER_AXIS_LEFTX), stickAxis(SDL_CONTROLLER_AXIS_RIGHTX)},
        {stickAxis(SDL_CONTROLLER_AXIS_LEFTY), stickAxis(SDL_CONTROLLER_AXIS_RIGHTY)},
    };
    for (const auto& pair : pairs)
    {
        if (pair[0] >= 0 && pair[1] >= 0 && pair[0] != pair[1])
        {
            twin[pair[0]] = pair[1];
            twin[pair[1]] = pair[0];
        }
    }
}

// Marks the stick direction of a joystick binding (see MapButton.h for the
// encoding). Directions bound to something themselves aren't mirrored.
inline void MarkBoundStickAxis(int binding, bool bound[16][2])
{
    if (binding == -1 || !(binding & 0x10000)) return;
    const int dir = (binding >> 20) & 0xF;
    if (dir < 2)
        bound[(binding >> 24) & 0xF][dir] = true;
}

// The value a binding to axis `axis` in direction `dir` (0 = +, 1 = -, 2 =
// trigger) sees: its own axis, or the other stick's if that one is pushed
// further the same way.
inline Sint16 StickAxisValue(SDL_Joystick* joystick, int axis, int dir,
                             bool enabled, const int twin[16], const bool bound[16][2])
{
    Sint16 value = SDL_JoystickGetAxis(joystick, axis);
    const int other = twin[axis];
    if (enabled && dir < 2 && other >= 0 && !bound[other][dir])
    {
        const Sint16 otherValue = SDL_JoystickGetAxis(joystick, other);
        value = (dir == 0) ? std::max(value, otherValue) : std::min(value, otherValue);
    }
    return value;
}

}
