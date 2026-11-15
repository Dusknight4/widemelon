// MultiMelon Wifi: the "M3DS" link on the 3DS (see Link3ds.h)
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Link3ds.h"

#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "Log.h"

namespace Link3ds
{
namespace
{
constexpr u64 kPeerTimeoutMs = 15000;   // a console quiet this long is gone
constexpr u64 kAwakeAfterMs = 20000;    // hellos (and the link) go on this long after the game's wireless stops
constexpr size_t kHeader = 22;
constexpr u32 kSocBufferSize = 0x20000;

using Mac = std::array<u8, 6>;

struct Peer
{
    sockaddr_in addr {};
    u64 lastHeardMs = 0;
};

LightLock g_lock;           // the state, the peers and the queue
bool g_lockReady = false;
bool g_running = false;
Thread g_thread = nullptr;
u32* g_socBuffer = nullptr;
int g_sock = -1;
Config g_config;
u32 g_session = 0;
FrameFn g_onFrame;
std::function<bool()> g_awake;
std::function<void(u64)> g_tick;
std::vector<sockaddr_in> g_helloTo;
std::map<Mac, Peer> g_peers;
std::deque<std::vector<u8>> g_queue;    // whole messages, with their destination MAC at [12..17]
unsigned g_framesOut = 0, g_framesIn = 0;

struct Locked
{
    Locked() { LightLock_Lock(&g_lock); }
    ~Locked() { LightLock_Unlock(&g_lock); }
};

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
            t.sin_port = htons((u16)std::atoi(one.c_str() + colon + 1));
            if (inet_aton(one.substr(0, colon).c_str(), &t.sin_addr)) out.push_back(t);
        }
        at = end + 1;
    }
    return out;
}

std::vector<sockaddr_in> BroadcastTargets()
{
    std::vector<sockaddr_in> to = g_helloTo;
    if (!g_helloTo.empty() || g_config.loopback) return to;
    auto add = [&](u32 a)
    {
        for (const sockaddr_in& t : to) if (t.sin_addr.s_addr == a) return;
        sockaddr_in t {};
        t.sin_family = AF_INET;
        t.sin_port = htons(g_config.port);
        t.sin_addr.s_addr = a;
        to.push_back(t);
    };
    add(htonl(INADDR_BROADCAST));
    in_addr ip {}, mask {}, bcast {};
    if (R_SUCCEEDED(SOCU_GetIPInfo(&ip, &mask, &bcast)) && bcast.s_addr) add(bcast.s_addr);
    else if (ip.s_addr && mask.s_addr) add(ip.s_addr | ~mask.s_addr);
    return to;
}

void SendTo(const u8* msg, size_t len, const sockaddr_in& to)
{
    sendto(g_sock, msg, len, 0, (const sockaddr*)&to, sizeof(to));
}

void HelloRound()
{
    u8 hello[17] = {'M', '3', 'D', 'S', 1, 0};
    std::memcpy(hello + 6, g_config.mac, 6);
    std::memcpy(hello + 12, &g_session, 4);
    hello[16] = 1;
    std::vector<sockaddr_in> to = BroadcastTargets();
    {
        Locked l;
        for (const auto& p : g_peers) to.push_back(p.second.addr);
    }
    for (const sockaddr_in& t : to) SendTo(hello, sizeof(hello), t);
}

void Heard(const Mac& mac, const sockaddr_in& from)
{
    Locked l;
    Peer& p = g_peers[mac];
    if (p.lastHeardMs == 0)
        LogF("link: console %02X:%02X:%02X:%02X:%02X:%02X at %s:%u\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port));
    p.addr = from;
    p.lastHeardMs = osGetTime();
}

void Receive(const u8* msg, size_t len, const sockaddr_in& from)
{
    if (len < 6 || std::memcmp(msg, "M3DS", 4) != 0 || msg[4] != 1) return;
    if (msg[5] == 0 && len >= 17)
    {
        u32 session;
        std::memcpy(&session, msg + 12, 4);
        if (session == g_session || std::memcmp(msg + 6, g_config.mac, 6) == 0) return;   // (our own echo)
        Mac mac;
        std::memcpy(mac.data(), msg + 6, 6);
        Heard(mac, from);
    }
    else if (msg[5] == 1 && len >= kHeader)
    {
        u16 n;
        std::memcpy(&n, msg + 20, 2);
        if (kHeader + n > len || std::memcmp(msg + 6, g_config.mac, 6) == 0) return;
        Mac src;
        std::memcpy(src.data(), msg + 6, 6);
        Heard(src, from);
        static const u8 everyone[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (std::memcmp(msg + 12, g_config.mac, 6) != 0 && std::memcmp(msg + 12, everyone, 6) != 0) return;
        g_framesIn++;
        if (g_onFrame) g_onFrame(msg + 6, msg + 12, msg[18], msg[19], msg + kHeader, n);
    }
}

// what's queued, to the destination's console (or to all)
void Flush()
{
    static const u8 everyone[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    while (true)
    {
        std::vector<u8> msg;
        std::vector<sockaddr_in> to;
        {
            Locked l;
            if (g_queue.empty()) return;
            msg.swap(g_queue.front());
            g_queue.pop_front();
            Mac d;
            std::memcpy(d.data(), &msg[12], 6);
            auto it = g_peers.find(d);
            if (std::memcmp(d.data(), everyone, 6) != 0 && it != g_peers.end()) to.push_back(it->second.addr);
            else for (const auto& p : g_peers) to.push_back(p.second.addr);
        }
        for (const sockaddr_in& t : to) SendTo(msg.data(), msg.size(), t);
    }
}

void Run(void*)
{
    std::vector<u8> buf(4096);
    u64 nextHello = 0, awakeUntil = osGetTime() + kAwakeAfterMs;
    while (true)
    {
        const u64 now = osGetTime();
        if (g_awake && g_awake()) awakeUntil = now + kAwakeAfterMs;
        if (now >= awakeUntil)
        {
            Locked l;
            g_running = false;      // (Start makes a new thread)
            g_queue.clear();
            break;
        }
        if (now >= nextHello)
        {
            nextHello = now + 1000;
            HelloRound();
            Locked l;
            for (auto it = g_peers.begin(); it != g_peers.end();)
                it = (now - it->second.lastHeardMs > kPeerTimeoutMs) ? g_peers.erase(it) : std::next(it);
        }
        Flush();
        if (g_tick) g_tick(now);

        pollfd p {g_sock, POLLIN, 0};
        if (poll(&p, 1, 4) <= 0 || !(p.revents & POLLIN)) continue;
        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom(g_sock, buf.data(), buf.size(), 0, (sockaddr*)&from, &fromLen);
        if (n > 0) Receive(buf.data(), (size_t)n, from);
    }
    close(g_sock);
    g_sock = -1;
    socExit();
    LogF("link: off (the game's wireless has been off for %u s)\n", (unsigned)(kAwakeAfterMs / 1000));
}
}

bool Start(const Config& config, FrameFn onFrame, std::function<bool()> awake, std::function<void(u64)> tick)
{
    if (!g_lockReady)
    {
        LightLock_Init(&g_lock);
        g_lockReady = true;
    }
    {
        Locked l;
        if (g_running) return true;
    }
    if (g_thread)
    {
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = nullptr;
    }
    g_config = config;
    g_onFrame = std::move(onFrame);
    g_awake = std::move(awake);
    g_tick = std::move(tick);
    g_helloTo = ParseTargets(config.helloTo);
    g_session = (u32)svcGetSystemTick() ^ (u32)osGetTime() * 2654435761u;
    g_peers.clear();

    if (!g_socBuffer) g_socBuffer = (u32*)memalign(0x1000, kSocBufferSize);
    if (!g_socBuffer)
    {
        LogF("link: no memory for soc:U\n");
        return false;
    }
    Result r = socInit(g_socBuffer, kSocBufferSize);
    if (R_FAILED(r))
    {
        LogF("link: socInit %08lX\n", r);
        return false;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0)
    {
        LogF("link: socket %d\n", g_sock);
        socExit();
        return false;
    }
    int yes = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    addr.sin_addr.s_addr = htonl(config.loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(g_sock, (const sockaddr*)&addr, sizeof(addr)) != 0)
    {
        LogF("link: can't bind port %u\n", (unsigned)config.port);
        close(g_sock);
        g_sock = -1;
        socExit();
        return false;
    }
    in_addr ip {}, mask {}, bcast {};
    SOCU_GetIPInfo(&ip, &mask, &bcast);
    LogF("link: on, port %u, this 3DS %s, MAC %02X:%02X:%02X:%02X:%02X:%02X\n", (unsigned)config.port, inet_ntoa(ip),
         config.mac[0], config.mac[1], config.mac[2], config.mac[3], config.mac[4], config.mac[5]);
    {
        Locked l;
        g_running = true;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(Run, nullptr, 0x4000, prio > 0x19 ? 0x19 : prio, -2, false);
    if (!g_thread)
    {
        LogF("link: no thread\n");
        Locked l;
        g_running = false;
        return false;
    }
    return true;
}

bool Running()
{
    if (!g_lockReady) return false;
    Locked l;
    return g_running;
}

void Send(const u8 dst[6], u8 type, u8 channel, const u8* data, size_t len)
{
    if (len > 0xFFFF || !g_lockReady) return;
    std::vector<u8> msg(kHeader + len);
    std::memcpy(msg.data(), "M3DS", 4);
    msg[4] = 1;
    msg[5] = 1;
    std::memcpy(&msg[6], g_config.mac, 6);
    std::memcpy(&msg[12], dst, 6);
    msg[18] = type;
    msg[19] = channel;
    const u16 n = (u16)len;
    std::memcpy(&msg[20], &n, 2);
    if (len) std::memcpy(&msg[kHeader], data, len);
    Locked l;
    if (!g_running) return;
    g_queue.push_back(std::move(msg));
    g_framesOut++;
}

Stats GetStats()
{
    Stats s;
    if (!g_lockReady) return s;
    Locked l;
    s.peers = (int)g_peers.size();
    s.framesOut = g_framesOut;
    s.framesIn = g_framesIn;
    return s;
}

}
