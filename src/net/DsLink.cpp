/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "DsLink.h"
#include "BridgeMP.h"
#include "DsBridge.h"
#include "OnlineLocal.h"
#include "NDS.h"
#include "Platform.h"
#include "wmhle/wmhle.h"
#include "wmhle/wmpeers.h"

#ifndef DSLINK_LOG_FILE
#define DSLINK_LOG_FILE "WideMelon bridge log.txt"
#endif

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace DsLink
{
namespace
{
using Socket = uintptr_t;
constexpr Socket kNoSocket = ~(Socket)0;

double NowMs()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::mutex g_lock;                  // the socket's life (Start, Stop)
std::mutex g_peerLock;              // g_peers
Socket g_sock = kNoSocket;
std::thread g_rx;
std::atomic<bool> g_stop {false};
std::atomic<bool> g_on {false};
bool g_portTaken = false;
// the others on the network (DSes and MultiMelons): everyone hears everyone, as on the air
WmPeers g_peers {};
std::atomic<int> g_nDs {0}, g_nPc {0}, g_nPeers {0};    // (for the status)
std::atomic<double> g_lastHeard {-1e9};                 // anything from anyone
// (the emulator thread) the game's wireless on, and for 20 s after: the link is on the network
// until then (as the DS's radio, which then rests)
std::atomic<double> g_awakeUntil {-1e9};
bool g_alwaysAwake = false;
constexpr double kAwakeAfterMs = 20000;
bool Awake() { return g_alwaysAwake || NowMs() < g_awakeUntil; }

// where hellos go by broadcast: this port on every network, or (tests) these
uint16_t g_port = 7071;
bool g_loopback = false;
std::vector<sockaddr_in> g_helloTo;
int (*g_sendHook)(uintptr_t, const uint8_t*, int, const void*, int) = nullptr;

std::mutex g_inLock;                // what came from the others, for wmhle
std::deque<std::vector<uint8_t>> g_inbox;
std::atomic<bool> g_inboxFull {false};

std::mutex g_countLock;
Counts g_total, g_window;

// the bridge log (the receiver and the emulator thread both write it; capped)
std::mutex g_logLock;
FILE* g_log = nullptr;
long g_logBytes = 0;
void (*g_logHook)(const char*) = nullptr;

void LogLine(const char* prefix, const char* text)
{
    std::lock_guard l(g_logLock);
    if (g_logHook)
    {
        if (*prefix) { std::string t = std::string(prefix) + text; g_logHook(t.c_str()); }
        else g_logHook(text);
    }
    if (g_logBytes > 16 * 1024 * 1024) return;
    if (!g_log) g_log = std::fopen(DSLINK_LOG_FILE, "a");
    if (!g_log) return;
    g_logBytes += std::fprintf(g_log, "[%.1f s] %s%s\n", NowMs() / 1000.0, prefix, text);
    std::fflush(g_log);
}
void PeersLog(void*, const char* text) { LogLine("", text); }

void CloseSock(Socket s)
{
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

// (emulator thread) the console wmhle stands in for, and this session's MAC address
melonDS::NDS* g_nds = nullptr;
uint8_t g_mac[6];
double g_frameUs = 0;               // the link's time in the frame so far
double g_lastReport = 0;

sockaddr_in AddrOf(const WmPeer& p)
{
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = p.ip;
    a.sin_port = htons(p.port);
    return a;
}

void RawSend(const sockaddr_in& to, const uint8_t* msg, size_t len)
{
    if (g_sendHook) g_sendHook(g_sock, msg, (int)len, &to, (int)sizeof(to));
    else sendto(g_sock, (const char*)msg, (int)len, 0, (const sockaddr*)&to, sizeof(to));
}

// a message of wmhle's: to the console it names, or to everyone (see wmpeers.h)
void SendRouted(const uint8_t* msg, size_t len)
{
    sockaddr_in to[WMPEERS_MAX];
    int n = 0;
    {
        std::lock_guard l(g_peerLock);
        int idx[WMPEERS_MAX];
        n = wmpeersRoute(&g_peers, msg, (unsigned)len, idx, WMPEERS_MAX);
        for (int i = 0; i < n; i++) to[i] = AddrOf(g_peers.p[idx[i]]);
    }
    for (int i = 0; i < n; i++) RawSend(to[i], msg, len);
}

std::string IpText(const sockaddr_in& a)
{
    char ip[64] = "?", t[80];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    std::snprintf(t, sizeof(t), "%s:%d", ip, ntohs(a.sin_port));
    return t;
}

// (tests) "ip:port[,ip:port...]"
std::vector<sockaddr_in> ParseTargets(const std::string& list)
{
    std::vector<sockaddr_in> out;
    size_t at = 0;
    while (at < list.size())
    {
        size_t end = list.find(',', at);
        if (end == std::string::npos) end = list.size();
        const std::string one = list.substr(at, end - at);
        const size_t colon = one.rfind(':');
        sockaddr_in t {};
        t.sin_family = AF_INET;
        t.sin_port = htons(colon == std::string::npos ? 7071 : (uint16_t)std::atoi(one.c_str() + colon + 1));
        if (inet_pton(AF_INET, one.substr(0, colon).c_str(), &t.sin_addr) == 1) out.push_back(t);
        at = end + 1;
    }
    return out;
}

// where our hello goes by broadcast, as the DS's: every network this PC is on (the general
// broadcast, which Windows may send on one network only, and each network's own)
std::vector<sockaddr_in> BroadcastTargets()
{
    std::vector<sockaddr_in> to = g_helloTo;
    auto add = [&](uint32_t a)      // (network order)
    {
        for (const sockaddr_in& t : to) if (t.sin_addr.s_addr == a) return;
        sockaddr_in t {};
        t.sin_family = AF_INET;
        t.sin_port = htons(g_port);
        t.sin_addr.s_addr = a;
        to.push_back(t);
    };
    if (!g_helloTo.empty() || g_loopback) return to;
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

// once a second while on the network: our hello by broadcast (for the ones that don't know us
// yet) and to each one we know (so a lost broadcast loses no one)
void HelloRound()
{
    uint8_t hello[WMPEERS_HELLO_LEN];
    std::vector<sockaddr_in> to = BroadcastTargets();
    {
        std::lock_guard l(g_peerLock);
        wmpeersHello(&g_peers, hello);
        for (const WmPeer& p : g_peers.p)
        {
            if (!p.used) continue;
            const sockaddr_in a = AddrOf(p);
            bool dup = false;
            for (const sockaddr_in& t : to) dup |= t.sin_addr.s_addr == a.sin_addr.s_addr && t.sin_port == a.sin_port;
            if (!dup) to.push_back(a);
        }
    }
    for (const sockaddr_in& t : to) RawSend(t, hello, sizeof(hello));
}

void CountPeers()
{
    int ds = 0, pc = 0, all = 0;
    for (const WmPeer& p : g_peers.p) if (p.used) { all++; ds += p.kind == 'D'; pc += p.kind == 'M'; }
    g_nDs = ds; g_nPc = pc; g_nPeers = all;
}

void RxLoop()
{
    std::vector<uint8_t> buf(4096);
    double lastHello = -1e9, last = NowMs();
    double awakeMs = 0;                 // a clock that runs only while we're on the network (as the DS's)
    bool awakeWas = false;
    while (!g_stop)
    {
        const double now = NowMs();
        const bool awake = Awake();
        if (awake) awakeMs += now - last;
        last = now;
        if (awake != awakeWas)
        {
            awakeWas = awake;
            if (awake) lastHello = -1e9;
            LogLine("", awake ? "the game's wireless is on: on the network (hellos by broadcast and to each one we know)"
                              : "the game's wireless off for 20 s: resting (no hellos, nothing taken in)");
        }
        if (awake && now - lastHello >= 1000)
        {
            lastHello = now;
            {
                std::lock_guard l(g_peerLock);
                wmpeersExpire(&g_peers, (uint32_t)awakeMs);
                CountPeers();
            }
            HelloRound();
        }

        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        const int n = (int)recvfrom(g_sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fromLen);
        if (n < 40) continue;
        const uint32_t ip = from.sin_addr.s_addr;
        const uint16_t port = ntohs(from.sin_port);
        const bool isLog = wmpeersIsLog(buf.data(), (unsigned)n), isHello = wmpeersIsHello(buf.data(), (unsigned)n);
        int idx;
        {
            std::lock_guard l(g_peerLock);
            // resting: as a DS with its radio off, nothing comes in (but a known one's log lines)
            if (!awake) idx = isLog ? wmpeersFind(&g_peers, ip, port) : -1;
            else idx = wmpeersRx(&g_peers, ip, port, buf.data(), (unsigned)n, (uint32_t)awakeMs);
            if (idx >= 0) CountPeers();
        }
        if (idx < 0) continue;
        g_lastHeard = NowMs();
        if (isHello)
        {
            std::lock_guard l(g_countLock);
            g_total.HellosIn++; g_window.HellosIn++;
            continue;
        }
        if (isLog)
        {
            // a DS's own log, each line as "[DS 10.0.0.65] ..."
            char ip4[32] = "?";
            inet_ntop(AF_INET, &from.sin_addr, ip4, sizeof(ip4));
            const std::string prefix = std::string("[DS ") + ip4 + "] ";
            const unsigned len = buf[38] | (unsigned)buf[39] << 8;
            std::string text((const char*)buf.data() + 40, std::min<size_t>(len, (size_t)n - 40));
            size_t at = 0;
            while (at < text.size())
            {
                size_t nl = text.find('\n', at);
                if (nl == std::string::npos) nl = text.size();
                if (nl > at) LogLine(prefix.c_str(), text.substr(at, nl - at).c_str());
                at = nl + 1;
            }
            continue;
        }
        {
            std::lock_guard l(g_countLock);
            g_total.MsgsIn++; g_window.MsgsIn++;
        }
        std::lock_guard l(g_inLock);
        if (g_inbox.size() < 1024) g_inbox.emplace_back(buf.data(), buf.data() + n);
        g_inboxFull = true;
    }
}

// ---- wmhle's host ----
uint8_t* HostRam(uint32_t a) { return g_nds->MainRAM + (a & g_nds->MainRAMMask); }
void HostToArm9(uint32_t w) { g_nds->InjectArm7Fifo(w); }
void HostSend(const uint8_t* msg, unsigned len)
{
    // (resting, as a DS with its radio off, nothing goes out; with no one known, a beacon
    // reaches no one, as on the air)
    if (!Awake()) return;
    SendRouted(msg, len);
    std::lock_guard l(g_countLock);
    g_total.MsgsOut++; g_window.MsgsOut++;
}
void Drain()
{
    if (!g_inboxFull) return;
    std::deque<std::vector<uint8_t>> in;
    {
        std::lock_guard l(g_inLock);
        in.swap(g_inbox);
        g_inboxFull = false;
    }
    for (auto& m : in) wmhleBridgeRx(m.data(), (unsigned)m.size());
}
void HostPoll() { Drain(); }
bool HostWaitArm9() { return false; }    // (the ARM9 runs between our calls: the rest goes on the next line)
uint32_t HostNowUs() { return (uint32_t)(NowMs() * 1000.0); }
void HostLog(const char* t) { LogLine("[WM] ", t); }
// the IPC FIFO to the ARM9 is full: wmhle waits rather than hand a callback over (the core drops a
// word injected into a full FIFO, and wmhle would then wait for good for the ARM9 to take that
// callback: at a link battle's faint, every callback after it stalled and the link died)
bool HostArm9FifoFull()
{
    if (!g_nds->Arm7FifoFull()) return false;
    std::lock_guard l(g_countLock);
    g_total.FifoFull++; g_window.FifoFull++;
    return true;
}
const WmHleHost g_host = { HostRam, HostToArm9, HostSend, HostPoll, HostWaitArm9, HostNowUs, g_mac, HostLog, HostArm9FifoFull };

bool Tap(melonDS::NDS* nds, uint32_t val)
{
    if (nds != g_nds || (val & 0x1F) != 10) return false;
    wmhleRequest(val >> 6);
    return true;
}

void Scanline(melonDS::NDS* nds, uint32_t line)
{
    if (nds != g_nds) return;
    const double t0 = NowMs();
    Drain();
    if (line == 192) wmhleFrame();
    else wmhlePump();
    g_frameUs += (NowMs() - t0) * 1000.0;
    if (line == 192)
    {
        // the link's time in this frame: never a wait, only its own work
        std::lock_guard l(g_countLock);
        for (Counts* c : {&g_total, &g_window})
        {
            c->LinkUsMax = std::max(c->LinkUsMax, g_frameUs);
            c->LinkUsSum += g_frameUs;
            c->Frames++;
        }
        g_frameUs = 0;
    }
}

std::string PeersText()
{
    std::lock_guard l(g_peerLock);
    std::string s;
    for (const WmPeer& p : g_peers.p)
    {
        if (!p.used) continue;
        char d[48];
        wmpeersDescribe(&p, d);
        s += s.empty() ? "" : ", ";
        s += d;
    }
    return s.empty() ? "no one" : s;
}

void Report()
{
    // every 5 seconds while something happens: how the link is doing
    const double now = NowMs();
    if (now - g_lastReport < 5000) return;
    g_lastReport = now;
    Counts w;
    {
        std::lock_guard l(g_countLock);
        w = g_window;
        g_window = Counts();
    }
    if (!w.MsgsIn && !w.MsgsOut) return;
    char line[300];
    std::snprintf(line, sizeof(line), "DS link: messages out %u, in %u, hellos in %u; link time per frame %.0f us on average, longest %.0f us; FIFO to the game full %u times%s",
                  w.MsgsOut, w.MsgsIn, w.HellosIn, w.Frames ? w.LinkUsSum / w.Frames : 0.0, w.LinkUsMax, w.FifoFull,
                  now - g_lastHeard > 3000 ? "; everyone is silent" : "");
    LogLine("", line);
    LogLine("", ("DS link: on the network with " + PeersText()).c_str());
}
}

void SetUserEnabled(bool on)
{
    // the emulated wireless through BridgeMP instead: asked for, or its own test setup
    const char* mode = std::getenv("WIDEMELON_DS_LINK");
    const char* bridge = std::getenv("WIDEMELON_BRIDGE");
    const bool old = (mode && mode[0] == '0') || (bridge && bridge[0]);
    melonDS::BridgeMP::SetUserEnabled(on && old);
    if (!on || old) { Stop(); return; }
    if (Enabled()) return;
    Settings s;
    if (const char* p = std::getenv("WIDEMELON_BRIDGE_PORT")) s.Port = (uint16_t)std::atoi(p);
    s.Loopback = std::getenv("WIDEMELON_BRIDGE_LOOPBACK") != nullptr;
    if (const char* h = std::getenv("WIDEMELON_BRIDGE_HELLO_TO")) s.HelloTo = h;
    s.AlwaysAwake = std::getenv("WIDEMELON_BRIDGE_AWAKE") != nullptr;
    Start(s);
}

bool Start(const Settings& s)
{
    Stop();
    std::lock_guard l(g_lock);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const auto sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((Socket)sock == kNoSocket) { LogLine("", "DS link: no UDP socket"); return false; }
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(s.Loopback ? INADDR_LOOPBACK : INADDR_ANY);
    local.sin_port = htons(s.Port);
    if (bind(sock, (sockaddr*)&local, sizeof(local)) != 0)
    {
        CloseSock((Socket)sock);
        g_portTaken = true;
        char line[100];
        std::snprintf(line, sizeof(line), "DS link: UDP port %u is taken (another WideMelon?)", s.Port);
        LogLine("", line);
        Log(LogLevel::Error, "WideMelon %s\n", line);
        return false;
    }
#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv {0, 100000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    const int one = 1;      // (our hellos by broadcast)
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
    g_sock = (Socket)sock;
    g_portTaken = false;
    g_awakeUntil = -1e9;    // (resting until the game turns its wireless on)
    g_alwaysAwake = s.AlwaysAwake;
    g_port = s.Port;
    g_loopback = s.Loopback;
    g_helloTo = ParseTargets(s.HelloTo);
    g_sendHook = s.SendHook;
    {
        std::lock_guard c(g_countLock);
        g_total = g_window = Counts();
    }
    // a MAC address of this session's own (00:09:BF is Nintendo's; the firmware's may be anyone's)
    std::random_device rd;
    const uint8_t mac[6] = {0x00, 0x09, 0xBF, (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd()};
    std::memcpy(g_mac, mac, 6);
    uint8_t tag[4];
    for (uint8_t& b : tag) b = (uint8_t)rd();
    {
        std::lock_guard p(g_peerLock);
        g_peers.log = PeersLog;
        wmpeersInit(&g_peers, g_mac, 'M', tag);
        CountPeers();
    }
    g_stop = false;
    g_on = true;
    g_rx = std::thread(RxLoop);
    char line[200];
    std::snprintf(line, sizeof(line), "DS link (wmhle): UDP port %u%s; with the game's wireless on it plays with every DS and MultiMelon on this network", s.Port, s.Loopback ? " (this PC only)" : "");
    LogLine("", line);
    Log(LogLevel::Info, "WideMelon %s\n", line);
    return true;
}

void Stop()
{
    std::lock_guard l(g_lock);
    if (g_sock == kNoSocket) { g_on = false; return; }
    // the others end their side of a link with us at once, rather than after their timeouts
    {
        DsBridge::Msg m;
        m.Type = DsBridge::MsgType::Leave;
        std::memcpy(m.Mac, g_mac, 6);
        const std::vector<uint8_t> w = m.Serialize();
        std::vector<sockaddr_in> to;
        {
            std::lock_guard p(g_peerLock);
            for (const WmPeer& q : g_peers.p) if (q.used) to.push_back(AddrOf(q));
        }
        for (const sockaddr_in& t : to) RawSend(t, w.data(), w.size());
    }
    g_on = false;
    g_stop = true;
    if (g_rx.joinable()) g_rx.join();
    CloseSock(g_sock);
    g_sock = kNoSocket;
    {
        std::lock_guard p(g_peerLock);
        for (WmPeer& q : g_peers.p) q.used = false;
        CountPeers();
    }
    std::lock_guard l2(g_inLock);
    g_inbox.clear();
    g_inboxFull = false;
}

namespace
{
// when WideMelon closes: the receiver thread stopped first (a std::thread still running when it's
// destroyed ends the program: closing WideMelon with the link on showed as a crash, 0xc0000409),
// and the others told we're gone. (Defined after the others in this file, so destroyed before them)
struct StopAtExit { ~StopAtExit() { Stop(); } } g_stopAtExit;
}

bool Enabled() { return g_on; }

std::string StatusText()
{
    if (g_portTaken) return "Its UDP port is taken (another WideMelon?)";
    if (!g_on) return "Off";
    if (!Awake()) return g_nPeers ? "Resting (the game's wireless is off)" : "Waiting for the game to use its wireless (the Union Room...)";
    if (!g_nPeers) return "Looking for DSes and other MultiMelons on this network";
    if (NowMs() - g_lastHeard > 3000) return "The others have gone quiet";
    const int ds = g_nDs, pc = g_nPc, other = g_nPeers - ds - pc;
    std::string s = "On the network with ";
    auto part = [&](int n, const char* one, const char* many) {
        if (!n) return;
        if (s.back() != ' ') s += ", ";
        s += std::to_string(n) + " " + (n == 1 ? one : many);
    };
    part(ds, "DS", "DSes");
    part(pc, "other MultiMelon", "other MultiMelons");
    part(other, "other", "others");
    return s;
}

void SetLogHook(void (*hook)(const char*)) { std::lock_guard l(g_logLock); g_logHook = hook; }

Counts GetCounts() { std::lock_guard l(g_countLock); return g_total; }

void Frame(melonDS::NDS* nds)
{
    // ("Online as Local" has the console's wireless while it's on)
    const bool on = Enabled() && !OnlineLocal::Enabled();
    if (on && (nds != g_nds || nds->ScanlineHook != Scanline))
    {
        if (g_nds && g_nds != nds && g_nds->ScanlineHook == Scanline) { g_nds->PxiTap = nullptr; g_nds->ScanlineHook = nullptr; }
        g_nds = nds;
        g_frameUs = 0;
        wmhleInit(&g_host);
        nds->PxiTap = Tap;
        nds->ScanlineHook = Scanline;
        LogLine("", "the game's wireless goes to the DS link (wmhle) now");
    }
    else if (!on && g_nds)
    {
        if (g_nds == nds && nds->ScanlineHook == Scanline) { nds->PxiTap = nullptr; nds->ScanlineHook = nullptr; }
        g_nds = nullptr;
    }
    if (!on) return;
    // on the network while the game has its wireless on, and for 20 s after (as the DS's radio)
    if (wmhleActive()) g_awakeUntil = NowMs() + kAwakeAfterMs;
    Report();
    // (diagnostics) a callback the game hasn't taken for a second (CALLBACK_CONTROL bit 0 still set:
    // every callback after it waits): the game's state now and a second later, into the game watch
    // files (HG link battles stalled like this at a faint)
    static unsigned busyFrames = 0, snapshots = 0;
    busyFrames = (nds->MainRAM[0x027FFF96 & nds->MainRAMMask] & 1) ? busyFrames + 1 : 0;
    if ((busyFrames == 60 || busyFrames == 120) && snapshots < 6)
    {
        snapshots++;
        char why[100];
        std::snprintf(why, sizeof why, "DS link: a callback not taken by the game for %u frames", busyFrames);
        LogLine("", why);
        nds->DebugSnapshot(why);
    }
}

}
