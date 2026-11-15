// MultiMelon: "Wifi as Local Multiplayer" for 3DS games (the UDS service over UdsLink).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace ThreeDSNet
{

// The 3DS core's local wireless goes over the network (UdsLink.h) from then on, as this console:
// its MAC address is userDir/mac.txt (made the first time, a Nintendo one with random last 3 bytes).
// Call it before a game starts. ports: how many ports from port on it may take (another MultiMelon on
// this PC has the first) and says hello on. helloTo/loopback: for tests (UdsLink::Config). A console that
// goes quiet or restarts is dropped from this console's network, as a 3DS drops one that stops answering.
bool Start(const std::string& userDir, uint16_t port, const std::string& helloTo = "", bool loopback = false,
           int ports = 1);
void Stop();
// the port it got (0: off)
uint16_t Port();

}
