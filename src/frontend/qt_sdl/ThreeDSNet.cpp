// MultiMelon: "Wifi as Local Multiplayer" for 3DS games (see ThreeDSNet.h).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ThreeDSNet.h"

#include <cstdio>
#include <memory>
#include <random>

#include "UdsLink.h"

#include "core/hle/service/nwm/uds_link.h"

namespace ThreeDSNet
{

namespace
{
// this console's MAC address: from mac.txt, or a new one saved there
Network::MacAddress LoadMac(const std::string& userDir)
{
    Network::MacAddress mac {0x40, 0xF4, 0x07, 0, 0, 0};    // (a Nintendo prefix, as a 3DS's)
    const std::string path = userDir + (userDir.empty() || userDir.back() == '/' || userDir.back() == '\\' ? "" : "/") + "mac.txt";
    if (FILE* f = std::fopen(path.c_str(), "r"))
    {
        unsigned b[6];
        const bool ok = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6;
        std::fclose(f);
        if (ok)
        {
            for (int i = 0; i < 6; i++) mac[i] = (u8)b[i];
            return mac;
        }
    }
    std::random_device rd;
    for (int i = 3; i < 6; i++) mac[i] = (u8)rd();
    if (FILE* f = std::fopen(path.c_str(), "w"))
    {
        std::fprintf(f, "%02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        std::fclose(f);
    }
    return mac;
}
}

bool Start(const std::string& userDir, uint16_t port, const std::string& helloTo, bool loopback)
{
    Stop();
    auto link = std::make_shared<Service::NWM::UdsLink>();
    link->mac = LoadMac(userDir);
    link->send = [](const Network::WifiPacket& p) {
        UdsLink::Send(p.destination_address.data(), (uint8_t)p.type, p.channel, p.data.data(), p.data.size());
    };

    UdsLink::Config config;
    config.port = port;
    std::copy(link->mac.begin(), link->mac.end(), config.mac);
    config.helloTo = helloTo;
    config.loopback = loopback;
    const bool ok = UdsLink::Start(
        config,
        [](const uint8_t src[6], const uint8_t dst[6], uint8_t type, uint8_t channel, const uint8_t* data, size_t len) {
            Network::WifiPacket p;
            p.type = (Network::WifiPacket::PacketType)type;
            p.data.assign(data, data + len);
            std::copy(src, src + 6, p.transmitter_address.begin());
            std::copy(dst, dst + 6, p.destination_address.begin());
            p.channel = channel;
            Service::NWM::DeliverUdsPacket(p);
        },
        [] { return Service::NWM::UdsActive(); });
    if (ok) Service::NWM::SetUdsLink(link);
    return ok;
}

void Stop()
{
    UdsLink::Stop();
    Service::NWM::SetUdsLink(nullptr);
}

}
