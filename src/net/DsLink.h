/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef WIDEMELON_DSLINK_H
#define WIDEMELON_DSLINK_H

// "Connect a real DS (WiFi bridge)": local wireless with a real DS running the game under the
// modified nds-bootstrap, on the same network. As in "Online as Local", the game's wireless
// requests don't go to the emulated ARM7 wireless but to wmhle (the WM HLE the DS itself runs),
// and the two wmhles talk DsBridge messages to each other, here directly over UDP (port 7071).
// Neither side ever waits for the other: a slow network only makes the link slower (data a
// frame or two later), never the emulator. (BridgeMP, the emulated wireless talking to the DS,
// waited for the DS's reply in every round it hosted: frames lost on a slow network.)
//
// The DS says hello by broadcast until a WideMelon answers; WideMelon answers whoever writes to
// it, and says hello back once a second (the DS looks for it again after 15 s of silence). The
// DS's log lines ("[DS] ...") and the link's own go to "WideMelon bridge log.txt".
//
// Turn it on before the game uses its wireless (it's remembered, so normally from the start).

#include <cstdint>
#include <string>

namespace melonDS { class NDS; }

namespace DsLink
{
struct Settings
{
    uint16_t Port = 7071;
    bool Loopback = false;      // only this PC can reach it (tests)
};

// the Multiplayer menu's option: starts or stops the link. (WIDEMELON_DS_LINK=0: the emulated
// wireless through BridgeMP instead, as before; WIDEMELON_BRIDGE set: BridgeMP, for its tests)
void SetUserEnabled(bool on);

bool Start(const Settings& s);
void Stop();
bool Enabled();
std::string StatusText();
// (testing) also hand each line of the link's log to this
void SetLogHook(void (*hook)(const char*));

struct Counts
{
    uint32_t MsgsOut = 0, MsgsIn = 0, HellosIn = 0;
    uint32_t FifoFull = 0;      // times a callback waited: the IPC FIFO to the game was full
    double LinkUsMax = 0;       // the longest time the link took in one emulated frame (µs)
    double LinkUsSum = 0;       // in all frames (with Frames: the average)
    uint32_t Frames = 0;
};
Counts GetCounts();             // since Start

// the emulator thread, before each frame of the first instance: puts wmhle in the console's
// place for its wireless while on (again for a new console), takes it out when off. "Online
// as Local", when on, goes first
void Frame(melonDS::NDS* nds);
}

#endif
