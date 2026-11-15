// MultiMelon Wifi: the "M3DS" link on the 3DS (soc:U), as MultiMelon's UdsLink speaks it on the PC
// (src/frontend/qt_sdl/UdsLink.h has the protocol). One thread sends what's queued, says hello once a
// second while the game's wireless is on (and 20 s after), receives frames, and ticks the UDS service.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <3ds.h>

#include <functional>
#include <string>

namespace Link3ds
{

struct Config
{
    u16 port = 7072;
    u8 mac[6] {};
    std::string helloTo;    // "ip:port[,...]": hellos go there instead of by broadcast (tests)
    bool loopback = false;  // bind to 127.0.0.1 (tests in the emulator)
};

using FrameFn = std::function<void(const u8 src[6], const u8 dst[6], u8 type, u8 channel, const u8* data, size_t len)>;

// starts soc:U and the thread (once; later calls just keep it running). onFrame and tick run on that thread.
bool Start(const Config& config, FrameFn onFrame, std::function<bool()> awake, std::function<void(u64 nowMs)> tick);
bool Running();
// queues a frame (doesn't block; safe from any thread)
void Send(const u8 dst[6], u8 type, u8 channel, const u8* data, size_t len);

struct Stats { int peers = 0; unsigned framesOut = 0, framesIn = 0; };
Stats GetStats();

}
