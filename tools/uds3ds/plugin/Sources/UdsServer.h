// MultiMelon Wifi: serves the game's nwm::UDS sessions itself (uds_core over Link3ds) instead of the
// console's NWM module, so its local wireless goes over the WiFi network.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <3ds.h>

#include "Link3ds.h"

namespace UdsServer
{

// the link's settings (this console's MAC, port, test peers); before any session
void Configure(const Link3ds::Config& config);

// a new nwm::UDS session served by a thread of ours: the game gets its client handle
Result OpenSession(Handle* client);

// finds the game's srv GetServiceHandle (by its code: the 0x00050100 request) and hooks it, so asking for
// "nwm::UDS" opens one of our sessions; returns how many were hooked
int InstallHooks();

}
