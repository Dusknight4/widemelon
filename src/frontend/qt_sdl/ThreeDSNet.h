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
// Call it before a game starts. helloTo/loopback: for tests (UdsLink::Config).
bool Start(const std::string& userDir, uint16_t port, const std::string& helloTo = "", bool loopback = false);
void Stop();

}
