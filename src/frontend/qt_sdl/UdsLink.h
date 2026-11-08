// MultiMelon: 3DS local wireless (UDS) over a network, as the DS link carries DS wireless.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Every console on the network (MultiMelons, and real 3DSes running the UDS bridge module) hears every
// other, as 3DSes in the same room do: no pairing. A console says hello by broadcast once a second
// while its game's local wireless is on (and 20 s after); each 802.11 frame the UDS service sends goes
// to the console its destination MAC names, or to every console it knows (broadcast frames: beacons).
//
// UDP messages (little-endian):
//   0  "M3DS"   4  version (1)   5  kind: 0 hello, 1 frame
//   hello:  6 mac[6]  12 session u32  16 flags (bit 0: the game's wireless is on)
//   frame:  6 src mac[6]  12 dst mac[6]  18 type (Azahar's WifiPacket::PacketType)  19 channel
//           20 length u16  22 the 802.11 frame
namespace UdsLink
{

struct Config
{
    uint16_t port = 7072;
    uint8_t mac[6] {};
    // (tests) "ip:port[,ip:port...]": hellos go there instead of by broadcast
    std::string helloTo;
    bool loopback = false;      // (tests) bind to 127.0.0.1 only
};

using FrameFn = std::function<void(const uint8_t src[6], const uint8_t dst[6], uint8_t type, uint8_t channel,
                                   const uint8_t* data, size_t len)>;

// Opens the socket and its thread. Frames for this console (or for everyone) go to onFrame (on that
// thread); awake() says whether the game's wireless is on.
bool Start(const Config& config, FrameFn onFrame, std::function<bool()> awake);
void Stop();
bool Running();

// A frame to the console with that MAC (FF:FF:FF:FF:FF:FF: everyone). Doesn't block.
void Send(const uint8_t dst[6], uint8_t type, uint8_t channel, const uint8_t* data, size_t len);

// for the window title and logs: consoles heard in the last 15 s, frames out/in
struct Stats { int peers = 0; unsigned framesOut = 0, framesIn = 0; };
Stats GetStats();

}
