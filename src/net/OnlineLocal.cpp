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
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "OnlineLocal.h"
#include "OnlineRelay.h"
#include "NDS.h"
#include "Platform.h"
#include "wmhle/wmhle.h"

#ifndef OL_LOG_FILE
#define OL_LOG_FILE "WideMelon online log.txt"
#endif

using namespace OnlineProto;
using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace OnlineLocal
{

// ---- SHA-256 (for the room id) ----
namespace
{
struct Sha256
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    uint64_t len = 0;
    size_t fill = 0;

    static uint32_t Ror(uint32_t x, int n) { return x >> n | x << (32 - n); }
    void Block(const uint8_t* p)
    {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++)
        {
            const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++)
        {
            const uint32_t t1 = hh + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void Add(const void* data, size_t n)
    {
        const uint8_t* p = (const uint8_t*)data;
        len += n;
        while (n--)
        {
            buf[fill++] = *p++;
            if (fill == 64) { Block(buf); fill = 0; }
        }
    }
    void Finish(uint8_t out[32])
    {
        const uint64_t bits = len * 8;
        const uint8_t one = 0x80, zero = 0;
        Add(&one, 1);
        while (fill != 56) Add(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
        Add(l, 8);
        for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
    }
};
}

void RoomIdFrom(const std::string& name, const std::string& password, uint8_t out[16])
{
    Sha256 s;
    const char tag[] = "WideMelon Online as Local room";
    s.Add(tag, sizeof(tag));
    for (const std::string* p : {&name, &password})
    {
        const uint32_t n = (uint32_t)p->size();
        const uint8_t l[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
        s.Add(l, 4);
        s.Add(p->data(), p->size());
    }
    uint8_t full[32];
    s.Finish(full);
    std::memcpy(out, full, 16);
}

// ---- the link ----
namespace
{
using Socket = uintptr_t;
constexpr Socket kNoSocket = ~(Socket)0;

double NowMs()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::mutex g_lock;                 // Settings, State, Ping, the socket's life
Settings g_set;
std::atomic<State> g_state {State::Off};
std::atomic<int> g_ping {-1};
Socket g_sock = kNoSocket;
sockaddr_in g_server {};
uint32_t g_session = 0;
std::thread g_rx;
std::atomic<bool> g_stop {false};
OnlineRelay g_relay;

std::mutex g_inLock;               // what came from the friend, for wmhle
std::deque<std::vector<uint8_t>> g_inbox;
std::atomic<bool> g_inboxFull {false};

// (emulator thread only)
melonDS::NDS* g_nds = nullptr;
uint8_t g_mac[6];
FILE* g_log = nullptr;
void (*g_logHook)(const char*) = nullptr;
long g_logBytes = 0;

void CloseSock(Socket s)
{
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

void SendRaw(uint8_t type, const uint8_t* body, size_t n)
{
    uint8_t m[2048];
    if (HeaderSize + n > sizeof(m) || g_sock == kNoSocket) return;
    std::memcpy(m, "WMOL", 4);
    m[4] = Version;
    m[5] = type;
    std::memcpy(m + 6, g_set.RoomId, 16);
    if (n) std::memcpy(m + HeaderSize, body, n);
    sendto(g_sock, (const char*)m, (int)(HeaderSize + n), 0, (const sockaddr*)&g_server, sizeof(g_server));
}

void SendHello()
{
    const uint32_t t = (uint32_t)NowMs();
    uint8_t b[9] = {(uint8_t)(g_set.Host ? 0 : 1),
                    (uint8_t)g_session, (uint8_t)(g_session >> 8), (uint8_t)(g_session >> 16), (uint8_t)(g_session >> 24),
                    (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24)};
    SendRaw(Hello, b, sizeof(b));
}

void RxLoop()
{
    uint8_t buf[2048];
    double lastHello = -1e9, lastHeard = NowMs();
    while (!g_stop)
    {
        const double now = NowMs();
        if (now - lastHello >= 1000) { lastHello = now; SendHello(); }
        if (now - lastHeard > 5000 && g_state != State::NoRoom && g_state != State::RoomInUse) g_state = State::NoServer;
        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        const int n = (int)recvfrom(g_sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n < (int)HeaderSize || std::memcmp(buf, "WMOL", 4) != 0 || buf[4] != Version) continue;
        if (std::memcmp(buf + 6, g_set.RoomId, 16) != 0) continue;
        lastHeard = NowMs();
        const uint8_t* body = buf + HeaderSize;
        const int bodyLen = n - (int)HeaderSize;
        if (buf[5] == Status && bodyLen >= 6)
        {
            const uint32_t echo = body[2] | body[3] << 8 | body[4] << 16 | (uint32_t)body[5] << 24;
            if (echo) g_ping = (int)((uint32_t)NowMs() - echo);
            const State before = g_state;
            switch (body[0])
            {
                case Waiting: g_state = State::Waiting; break;
                case Together: g_state = State::Together; break;
                case NoRoom: g_state = State::NoRoom; break;
                case RoomInUse: g_state = State::RoomInUse; break;
            }
            if (g_state != before) Log(LogLevel::Info, "Online as Local: %s\n", StatusText().c_str());
        }
        else if (buf[5] == Data && bodyLen > 0)
        {
            std::lock_guard l(g_inLock);
            if (g_inbox.size() < 512) g_inbox.emplace_back(body, body + bodyLen);
            g_inboxFull = true;
        }
    }
}

// ---- wmhle's host ----
uint8_t* HostRam(uint32_t a) { return g_nds->MainRAM + (a & g_nds->MainRAMMask); }
void HostToArm9(uint32_t w) { g_nds->InjectArm7Fifo(w); }
void HostSend(const uint8_t* msg, unsigned len)
{
    // (until the friend is in the room there's nobody to hear it: beacons and scans find no one)
    if (g_state == State::Together) SendRaw(Data, msg, len);
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
void HostLog(const char* t)
{
    if (g_logHook) g_logHook(t);
    // wmhle's own log, to a file next to WideMelon (capped): for when something goes wrong
    if (g_logBytes > 8 * 1024 * 1024) return;
    if (!g_log) g_log = std::fopen(OL_LOG_FILE, "w");
    if (!g_log) return;
    g_logBytes += std::fprintf(g_log, "[%.1f s] %s\n", NowMs() / 1000.0, t);
    std::fflush(g_log);
}
const WmHleHost g_host = { HostRam, HostToArm9, HostSend, HostPoll, HostWaitArm9, HostNowUs, g_mac, HostLog, nullptr };

bool Tap(melonDS::NDS* nds, uint32_t val)
{
    if (nds != g_nds || (val & 0x1F) != 10) return false;
    wmhleRequest(val >> 6);
    return true;
}
void Scanline(melonDS::NDS* nds, uint32_t line)
{
    if (nds != g_nds) return;
    Drain();
    if (line == 192) wmhleFrame();
    else wmhlePump();
}
}

std::string StatusText()
{
    std::string s;
    switch (g_state.load())
    {
        case State::Off: return "Off";
        case State::Connecting: s = "Connecting to the room server..."; break;
        case State::NoServer: s = "The room server isn't answering (check its address, and that it's running)"; break;
        case State::Waiting: s = "In the room: waiting for your friend to join"; break;
        case State::Together: s = "Connected: your friend is in the room. Enter the Union Room now"; break;
        case State::NoRoom: s = "No room with that name and password is open: check both, or ask your friend to host first"; break;
        case State::RoomInUse: s = "Someone else is already using that room: pick another name or password"; break;
        case State::PortTaken: s = "Couldn't run the room server here: its UDP port is taken"; break;
    }
    const int p = g_ping;
    if (p >= 0 && (g_state == State::Waiting || g_state == State::Together)) s += " (server " + std::to_string(p) + " ms)";
    return s;
}

bool Start(const Settings& s)
{
    Stop();
    std::lock_guard l(g_lock);
    g_set = s;
    std::string host = s.Server;
    uint16_t port = DefaultPort;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) { port = (uint16_t)std::atoi(host.c_str() + colon + 1); host = host.substr(0, colon); }
    if (host.empty()) host = "127.0.0.1";
    if (s.RunServer && !g_relay.Start(port, [](const std::string& t) { Log(LogLevel::Info, "%s\n", t.c_str()); }))
    {
        g_state = State::PortTaken;
        return false;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
    {
        g_relay.Stop();
        g_state = State::NoServer;
        return false;
    }
    std::memcpy(&g_server, res->ai_addr, sizeof(sockaddr_in));
    freeaddrinfo(res);
    const auto sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((Socket)sock == kNoSocket) { g_relay.Stop(); g_state = State::NoServer; return false; }
#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv {0, 100000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    g_sock = (Socket)sock;
    std::random_device rd;
    g_session = rd() | 1;
    g_ping = -1;
    g_state = State::Connecting;
    g_stop = false;
    g_rx = std::thread(RxLoop);
    Log(LogLevel::Info, "Online as Local: %s %s:%u\n", s.Host ? "hosting at" : "joining at", host.c_str(), port);
    return true;
}

void Stop()
{
    std::lock_guard l(g_lock);
    if (g_sock != kNoSocket)
    {
        SendRaw(Bye, nullptr, 0);
        g_stop = true;
        if (g_rx.joinable()) g_rx.join();
        CloseSock(g_sock);
        g_sock = kNoSocket;
    }
    g_relay.Stop();
    g_state = State::Off;
    std::lock_guard l2(g_inLock);
    g_inbox.clear();
    g_inboxFull = false;
}

void SetLogHook(void (*hook)(const char*)) { g_logHook = hook; }

bool Enabled() { return g_state != State::Off; }
State GetState() { return g_state; }
int PingMs() { return g_ping; }

void Frame(melonDS::NDS* nds)
{
    const bool on = Enabled();
    if (on && nds != g_nds)
    {
        if (g_nds) { g_nds->PxiTap = nullptr; g_nds->ScanlineHook = nullptr; }
        g_nds = nds;
        // a MAC address of this session's own (two players' firmwares may well have the same one)
        std::random_device rd;
        const uint8_t mac[6] = {0x00, 0x09, 0xBF, (uint8_t)rd(), (uint8_t)rd(), (uint8_t)rd()};
        std::memcpy(g_mac, mac, 6);
        wmhleInit(&g_host);
        nds->PxiTap = Tap;
        nds->ScanlineHook = Scanline;
    }
    else if (!on && g_nds)
    {
        if (g_nds == nds) { nds->PxiTap = nullptr; nds->ScanlineHook = nullptr; }
        g_nds = nullptr;
    }
}

}
