// MultiMelon: 3DS local wireless (UDS) over a network (see UdsLink.h).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "UdsLink.h"

namespace UdsLink
{
namespace
{
using Socket = uintptr_t;
constexpr Socket kNoSocket = ~(Socket)0;
constexpr double kPeerTimeoutMs = 15000.0;    // a console quiet this long is gone
constexpr double kAwakeAfterMs = 20000.0;     // hellos go on this long after the game's wireless stops
constexpr size_t kHeader = 22;

using Mac = std::array<uint8_t, 6>;

struct Peer
{
    sockaddr_in addr {};
    double lastHeardMs = 0.0;
};

Socket g_sock = kNoSocket;
std::thread g_thread;
std::atomic<bool> g_run {false};
Config g_config;
uint32_t g_session = 0;
FrameFn g_onFrame;
std::function<bool()> g_awake;
std::vector<sockaddr_in> g_helloTo;

std::mutex g_peerLock;
std::map<Mac, Peer> g_peers;
std::atomic<unsigned> g_framesOut {0}, g_framesIn {0};

double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void CloseSocket(Socket s)
{
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

std::vector<sockaddr_in> ParseTargets(const std::string& list)
{
    std::vector<sockaddr_in> out;
    size_t at = 0;
    while (at < list.size())
    {
        size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        const std::string one = list.substr(at, end - at);
        const size_t colon = one.find(':');
        if (colon != std::string::npos)
        {
            sockaddr_in t {};
            t.sin_family = AF_INET;
            t.sin_port = htons((uint16_t)std::atoi(one.c_str() + colon + 1));
            if (inet_pton(AF_INET, one.substr(0, colon).c_str(), &t.sin_addr) == 1) out.push_back(t);
        }
        at = end + 1;
    }
    return out;
}

// where hellos go by broadcast: every network this PC is on (as the DS link: the general broadcast,
// which Windows may send on one network only, and each network's own)
std::vector<sockaddr_in> BroadcastTargets()
{
    std::vector<sockaddr_in> to = g_helloTo;
    if (!g_helloTo.empty() || g_config.loopback) return to;
    auto add = [&](uint32_t a)
    {
        for (const sockaddr_in& t : to) if (t.sin_addr.s_addr == a) return;
        sockaddr_in t {};
        t.sin_family = AF_INET;
        t.sin_port = htons(g_config.port);
        t.sin_addr.s_addr = a;
        to.push_back(t);
    };
    add(htonl(INADDR_BROADCAST));
#ifdef _WIN32
    INTERFACE_INFO list[32];
    DWORD bytes = 0;
    if (WSAIoctl((SOCKET)g_sock, SIO_GET_INTERFACE_LIST, nullptr, 0, list, sizeof(list), &bytes, nullptr, nullptr) == 0)
        for (DWORD i = 0; i < bytes / sizeof(INTERFACE_INFO); i++)
        {
            const u_long f = list[i].iiFlags;
            if (!(f & IFF_UP) || (f & IFF_LOOPBACK) || !(f & IFF_BROADCAST)) continue;
            add(list[i].iiAddress.AddressIn.sin_addr.s_addr | ~list[i].iiNetmask.AddressIn.sin_addr.s_addr);
        }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0)
    {
        for (const ifaddrs* i = ifs; i; i = i->ifa_next)
        {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_BROADCAST)) continue;
            add(((const sockaddr_in*)i->ifa_addr)->sin_addr.s_addr | ~((const sockaddr_in*)i->ifa_netmask)->sin_addr.s_addr);
        }
        freeifaddrs(ifs);
    }
#endif
    return to;
}

void SendTo(const uint8_t* msg, size_t len, const sockaddr_in& to)
{
    sendto((int)g_sock, (const char*)msg, (int)len, 0, (const sockaddr*)&to, sizeof(to));
}

// our hello: by broadcast (for consoles that don't know us yet) and to each one we know
void HelloRound(bool awake)
{
    uint8_t hello[17] = {'M', '3', 'D', 'S', 1, 0};
    std::memcpy(hello + 6, g_config.mac, 6);
    std::memcpy(hello + 12, &g_session, 4);
    hello[16] = awake ? 1 : 0;
    std::vector<sockaddr_in> to = BroadcastTargets();
    {
        std::lock_guard l(g_peerLock);
        for (const auto& [mac, peer] : g_peers) to.push_back(peer.addr);
    }
    for (const sockaddr_in& t : to) SendTo(hello, sizeof(hello), t);
}

void Heard(const Mac& mac, const sockaddr_in& from)
{
    std::lock_guard l(g_peerLock);
    Peer& p = g_peers[mac];
    p.addr = from;
    p.lastHeardMs = NowMs();
}

void Receive(const uint8_t* msg, size_t len, const sockaddr_in& from)
{
    if (len < 6 || std::memcmp(msg, "M3DS", 4) != 0 || msg[4] != 1) return;
    if (msg[5] == 0 && len >= 17)
    {
        uint32_t session;
        std::memcpy(&session, msg + 12, 4);
        if (session == g_session || std::memcmp(msg + 6, g_config.mac, 6) == 0) return;   // (our own echo)
        Mac mac;
        std::memcpy(mac.data(), msg + 6, 6);
        Heard(mac, from);
    }
    else if (msg[5] == 1 && len >= kHeader)
    {
        uint16_t n;
        std::memcpy(&n, msg + 20, 2);
        if (kHeader + n > len || std::memcmp(msg + 6, g_config.mac, 6) == 0) return;
        Mac src;
        std::memcpy(src.data(), msg + 6, 6);
        Heard(src, from);
        static const uint8_t everyone[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (std::memcmp(msg + 12, g_config.mac, 6) != 0 && std::memcmp(msg + 12, everyone, 6) != 0) return;
        g_framesIn++;
        if (g_onFrame) g_onFrame(msg + 6, msg + 12, msg[18], msg[19], msg + kHeader, n);
    }
}

void Run()
{
    std::vector<uint8_t> buf(4096);
    double nextHello = 0.0, awakeUntil = 0.0;
    while (g_run)
    {
        const double now = NowMs();
        if (g_awake && g_awake()) awakeUntil = now + kAwakeAfterMs;
        if (now >= nextHello)
        {
            nextHello = now + 1000.0;
            if (now < awakeUntil) HelloRound(true);
            std::lock_guard l(g_peerLock);
            for (auto it = g_peers.begin(); it != g_peers.end();)
                it = (now - it->second.lastHeardMs > kPeerTimeoutMs) ? g_peers.erase(it) : std::next(it);
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET((int)g_sock, &set);
        timeval tv {0, 50000};
        if (select((int)g_sock + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom((int)g_sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fromLen);
        if (n > 0) Receive(buf.data(), (size_t)n, from);
    }
}

}

bool Start(const Config& config, FrameFn onFrame, std::function<bool()> awake)
{
    Stop();
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_config = config;
    g_onFrame = std::move(onFrame);
    g_awake = std::move(awake);
    g_helloTo = ParseTargets(config.helloTo);
    g_session = std::random_device {}();
    {
        std::lock_guard l(g_peerLock);
        g_peers.clear();
    }

    const auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == (decltype(s))-1) return false;
    g_sock = (Socket)s;
    int yes = 1;
    setsockopt((int)g_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    addr.sin_addr.s_addr = htonl(config.loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind((int)g_sock, (const sockaddr*)&addr, sizeof(addr)) != 0)
    {
        CloseSocket(g_sock);
        g_sock = kNoSocket;
        return false;
    }
    g_run = true;
    g_thread = std::thread(Run);
    return true;
}

void Stop()
{
    if (!g_run) return;
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
    CloseSocket(g_sock);
    g_sock = kNoSocket;
}

bool Running() { return g_run; }

void Send(const uint8_t dst[6], uint8_t type, uint8_t channel, const uint8_t* data, size_t len)
{
    if (!g_run || len > 0xFFFF) return;
    std::vector<uint8_t> msg(kHeader + len);
    std::memcpy(msg.data(), "M3DS", 4);
    msg[4] = 1;
    msg[5] = 1;
    std::memcpy(&msg[6], g_config.mac, 6);
    std::memcpy(&msg[12], dst, 6);
    msg[18] = type;
    msg[19] = channel;
    const uint16_t n = (uint16_t)len;
    std::memcpy(&msg[20], &n, 2);
    if (len) std::memcpy(&msg[kHeader], data, len);

    static const uint8_t everyone[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<sockaddr_in> to;
    {
        std::lock_guard l(g_peerLock);
        Mac d;
        std::memcpy(d.data(), dst, 6);
        auto it = g_peers.find(d);
        if (std::memcmp(dst, everyone, 6) != 0 && it != g_peers.end()) to.push_back(it->second.addr);
        else for (const auto& [mac, peer] : g_peers) to.push_back(peer.addr);
    }
    for (const sockaddr_in& t : to) SendTo(msg.data(), msg.size(), t);
    g_framesOut++;
}

Stats GetStats()
{
    Stats s;
    {
        std::lock_guard l(g_peerLock);
        s.peers = (int)g_peers.size();
    }
    s.framesOut = g_framesOut;
    s.framesIn = g_framesIn;
    return s;
}

}
