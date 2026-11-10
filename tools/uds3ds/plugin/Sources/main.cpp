// MultiMelon Wifi: a Luma3DS plugin that carries a 3DS game's local wireless (UDS) over WiFi.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// First step: proves the plugin is loaded into the game (a line in sdmc:/mmwifi.log).

#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstdio>

namespace CTRPluginFramework
{

static void Log(const char* text)
{
    File f("/mmwifi.log", File::WRITE | File::CREATE | File::APPEND);
    if (f.IsOpen()) f.Write(text, std::strlen(text));
}

// called when the plugin starts, before the game runs
void PatchProcess(FwkSettings& settings)
{
    settings.WaitTimeToBoot = Seconds(0);
    char line[96];
    std::snprintf(line, sizeof(line), "MultiMelon Wifi plugin loaded, title %016llX\n", Process::GetTitleID());
    Log(line);
}

void OnProcessExit(void)
{
}

int main(void)
{
    Log("MultiMelon Wifi plugin main\n");
    Process::WaitForExit();
    return 0;
}

}
