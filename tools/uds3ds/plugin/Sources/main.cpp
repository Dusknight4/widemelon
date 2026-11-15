// MultiMelon Wifi: a Luma3DS plugin that carries a 3DS game's local wireless (UDS) over WiFi, to MultiMelon on
// a PC (its 3DS core's "Wifi as Local Multiplayer") and to other 3DSes running this plugin.
//
// The game's srv GetServiceHandle is hooked before the game runs; when the game asks for "nwm::UDS" it gets
// a session of ours (UdsServer), served by uds_core (the UDS service, as Azahar's) over the M3DS link on
// the WiFi network (Link3ds). Everything is logged to sdmc:/mmwifi.log.
//
// Optional settings, sdmc:/mmwifi.cfg (key=value lines): port=7072, peers=ip:port[,...] (hellos go there
// instead of by broadcast), loopback=1 (tests in the emulator). This console's MAC is kept in
// sdmc:/mmwifi_mac.txt (made up the first time, 40:F4:07:xx:xx:xx, as MultiMelon's 3ds/mac.txt).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Link3ds.h"
#include "Log.h"
#include "UdsServer.h"

namespace CTRPluginFramework
{
namespace
{
std::string ReadText(const char* path)
{
    File f(path, File::READ);
    if (!f.IsOpen()) return "";
    const u64 size = f.GetSize();
    if (size == 0 || size > 4096) return "";
    std::string s(size, '\0');
    if (f.Read(&s[0], (u32)size) != 0) return "";
    return s;
}

bool ParseMac(const std::string& text, u8 out[6])
{
    unsigned v[6];
    if (std::sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (u8)v[i];
    return true;
}

Link3ds::Config LoadConfig()
{
    Link3ds::Config c;
    const std::string cfg = ReadText("/mmwifi.cfg");
    size_t at = 0;
    while (at < cfg.size())
    {
        size_t end = cfg.find('\n', at);
        if (end == std::string::npos) end = cfg.size();
        std::string line = cfg.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "port") c.port = (u16)std::atoi(value.c_str());
        else if (key == "peers") c.helloTo = value;
        else if (key == "loopback") c.loopback = value == "1";
    }

    if (!ParseMac(ReadText("/mmwifi_mac.txt"), c.mac))
    {
        u64 seed = svcGetSystemTick() ^ osGetTime();
        c.mac[0] = 0x40;
        c.mac[1] = 0xF4;
        c.mac[2] = 0x07;
        for (int i = 3; i < 6; i++)
        {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            c.mac[i] = (u8)(seed >> 56);
        }
        char text[24];
        std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X\n", c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
        File f("/mmwifi_mac.txt", File::WRITE | File::CREATE | File::TRUNCATE);
        if (f.IsOpen()) f.Write(text, std::strlen(text));
    }
    return c;
}
}

// called when the plugin starts, before the game runs
void PatchProcess(FwkSettings& settings)
{
    settings.WaitTimeToBoot = Seconds(0);
    LogF("MultiMelon Wifi plugin, title %016llX\n", Process::GetTitleID());
    const Link3ds::Config config = LoadConfig();
    LogF("settings: port %u%s%s%s\n", (unsigned)config.port, config.helloTo.empty() ? "" : ", peers ", config.helloTo.c_str(),
         config.loopback ? ", loopback" : "");
    UdsServer::Configure(config);
    UdsServer::InstallHooks();
}

void OnProcessExit(void)
{
}

int main(void)
{
    Process::WaitForExit();
    return 0;
}

}
