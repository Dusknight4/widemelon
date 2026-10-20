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
std::mutex g_peerLock;              // g_peer (the receiver takes only this one)
Socket g_sock = kNoSocket;
std::thread g_rx;
std::atomic<bool> g_stop {false};
std::atomic<bool> g_on {false};
bool g_portTaken = false;
sockaddr_in g_peer {};
std::atomic<bool> g_peerKnown {false};
std::atomic<bool> g_remoteIsDs {false};
std::atomic<double> g_lastHeard {-1e9};

std::mutex g_inLock;                // what came from the DS, for wmhle
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
    if (g_logHook) g_logHook(text);
    if (g_logBytes > 16 * 1024 * 1024) return;
    if (!g_log) g_log = std::fopen(DSLINK_LOG_FILE, "a");
    if (!g_log) return;
    g_logBytes += std::fprintf(g_log, "[%.1f s] %s%s\n", NowMs() / 1000.0, prefix, text);
    std::fflush(g_log);
}

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

void SendTo(const uint8_t* msg, size_t len)
{
    if (!g_peerKnown || g_sock == kNoSocket) return;
    sockaddr_in to;
    {
        std::lock_guard l(g_peerLock);
        to = g_peer;
    }
    sendto(g_sock, (const char*)msg, (int)len, 0, (const sockaddr*)&to, sizeof(to));
}

void SendHello()
{
    DsBridge::Msg m;
    m.Type = DsBridge::MsgType::Hello;
    std::memcpy(m.Mac, g_mac, 6);
    const std::vector<uint8_t> w = m.Serialize();
    SendTo(w.data(), w.size());
}

void RxLoop()
{
    std::vector<uint8_t> buf(4096);
    double lastHello = -1e9;
    while (!g_stop)
    {
        // once a second to the DS, so it knows where we are (and that we're still here)
        const double now = NowMs();
        if (g_peerKnown && now - lastHello >= 1000) { lastHello = now; SendHello(); }

        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        const int n = (int)recvfrom(g_sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fromLen);
        DsBridge::Msg m;
        if (n <= 0 || !m.Parse(buf.data(), (size_t)n)) continue;

        // answer whoever writes to us: a DS back with another address is found by its next hello
        bool moved;
        {
            std::lock_guard l(g_peerLock);
            moved = !g_peerKnown || g_peer.sin_addr.s_addr != from.sin_addr.s_addr || g_peer.sin_port != from.sin_port;
            g_peer = from;
            g_peerKnown = true;
        }
        g_lastHeard = NowMs();
        if (moved)
        {
            char ip[64] = "?", line[120];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            std::snprintf(line, sizeof(line), "remote is %s:%d", ip, ntohs(from.sin_port));
            LogLine("", line);
            Log(LogLevel::Info, "WideMelon DS link: %s\n", line);
            lastHello = -1e9;   // (our hello at once: the DS learns where we are)
        }
        if (m.Type == DsBridge::MsgType::Hello)
        {
            std::lock_guard l(g_countLock);
            g_total.HellosIn++; g_window.HellosIn++;
            if (!g_remoteIsDs && m.Body.size() >= 4 && std::memcmp(m.Body.data(), "DSHW", 4) == 0)
            {
                g_remoteIsDs = true;
                LogLine("", "the remote is a real DS");
            }
            continue;
        }
        if (m.Type == DsBridge::MsgType::Log)
        {
            // the DS's own log, each line as "[DS] ..."
            std::string text(m.Body.begin(), m.Body.end());
            size_t at = 0;
            while (at < text.size())
            {
                size_t nl = text.find('\n', at);
                if (nl == std::string::npos) nl = text.size();
                if (nl > at) LogLine("[DS] ", text.substr(at, nl - at).c_str());
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
    // (until a DS has written to us there's nobody to send to: beacons and scans find no one)
    if (!g_peerKnown) return;
    SendTo(msg, len);
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
                  now - g_lastHeard > 3000 ? "; the remote is silent" : "");
    LogLine("", line);
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
    g_sock = (Socket)sock;
    g_portTaken = false;
    g_peerKnown = false;
    g_remoteIsDs = false;
    {
        std::lock_guard c(g_countLock);
        g_total = g_window = Counts();
    }
    // a MAC address of this session's own (00:09:BF is Nintendo's; the firmware's may be anyone's)
    std::random_device rd;
    const uint8_t mac[6] = {0x00, 0x09, 0xBF, (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd()};
    std::memcpy(g_mac, mac, 6);
    g_stop = false;
    g_on = true;
    g_rx = std::thread(RxLoop);
    char line[120];
    std::snprintf(line, sizeof(line), "DS link (wmhle): UDP port %u%s, waiting for a DS to say hello", s.Port, s.Loopback ? " (this PC only)" : "");
    LogLine("", line);
    Log(LogLevel::Info, "WideMelon %s\n", line);
    return true;
}

void Stop()
{
    std::lock_guard l(g_lock);
    if (g_sock == kNoSocket) { g_on = false; return; }
    // the DS ends its side of a link at once, rather than after its timeouts
    if (g_peerKnown)
    {
        DsBridge::Msg m;
        m.Type = DsBridge::MsgType::Leave;
        std::memcpy(m.Mac, g_mac, 6);
        const std::vector<uint8_t> w = m.Serialize();
        SendTo(w.data(), w.size());
    }
    g_on = false;
    g_stop = true;
    if (g_rx.joinable()) g_rx.join();
    CloseSock(g_sock);
    g_sock = kNoSocket;
    g_peerKnown = false;
    std::lock_guard l2(g_inLock);
    g_inbox.clear();
    g_inboxFull = false;
}

namespace
{
// when WideMelon closes: the receiver thread stopped first (a std::thread still running when it's
// destroyed ends the program: closing WideMelon with the link on showed as a crash, 0xc0000409),
// and the DS told we're gone. (Defined after the others in this file, so destroyed before them)
struct StopAtExit { ~StopAtExit() { Stop(); } } g_stopAtExit;
}

bool Enabled() { return g_on; }

std::string StatusText()
{
    if (g_portTaken) return "Its UDP port is taken (another WideMelon?)";
    if (!g_on) return "Off";
    if (!g_peerKnown) return "Waiting for a DS on this network to say hello";
    if (NowMs() - g_lastHeard > 3000) return "The DS has gone quiet";
    return g_remoteIsDs ? "Connected to a real DS" : "Connected";
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
