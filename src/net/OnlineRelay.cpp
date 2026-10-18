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

#include <chrono>
#include <cstdlib>
#include <cstring>
#include "OnlineRelay.h"

using namespace OnlineProto;

static double RelayNowMs()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static bool SameAddr(const uint8_t* a, const void* b)
{
    const sockaddr_in* x = (const sockaddr_in*)a;
    const sockaddr_in* y = (const sockaddr_in*)b;
    return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
}

static std::string AddrText(const void* a)
{
    const sockaddr_in* s = (const sockaddr_in*)a;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, (void*)&s->sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(s->sin_port));
}

static void CloseSock(uintptr_t s)
{
#ifdef _WIN32
    closesocket((SOCKET)s);
#else
    close((int)s);
#endif
}

bool OnlineRelay::Open(uint16_t port)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const auto s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ((uintptr_t)s == ~(uintptr_t)0) return false;
    sockaddr_in local {};
    local.sin_family = AF_INET;
    // (WIDEMELON_ROOM_SERVER_LOOPBACK: only this PC can reach it; for tests)
    local.sin_addr.s_addr = htonl(std::getenv("WIDEMELON_ROOM_SERVER_LOOPBACK") ? INADDR_LOOPBACK : INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0) { CloseSock((uintptr_t)s); return false; }
#ifdef _WIN32
    DWORD tv = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv {0, 200000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    Sock = (uintptr_t)s;
    StopFlag = false;
    Rooms.clear();
    return true;
}

bool OnlineRelay::Start(uint16_t port, LogFn log)
{
    Stop();
    Log = std::move(log);
    if (!Open(port)) { Say("room server: UDP port " + std::to_string(port) + " is taken"); return false; }
    Say("room server: listening on UDP port " + std::to_string(port));
    Thread = std::thread([this] { Loop(); });
    return true;
}

bool OnlineRelay::RunHere(uint16_t port, LogFn log)
{
    Log = std::move(log);
    if (!Open(port)) { Say("room server: UDP port " + std::to_string(port) + " is taken"); return false; }
    Say("room server: listening on UDP port " + std::to_string(port));
    Loop();
    return true;
}

void OnlineRelay::Stop()
{
    StopFlag = true;
    if (Thread.joinable()) Thread.join();
    if (Sock != ~(uintptr_t)0) { CloseSock(Sock); Sock = ~(uintptr_t)0; }
}

void OnlineRelay::Loop()
{
    uint8_t buf[2048];
    while (!StopFlag)
    {
        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        const int n = (int)recvfrom(Sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n > 0) Handle(buf, n, &from);
        const double now = RelayNowMs();
        if (now - LastExpire >= 1000) { LastExpire = now; Expire(now); }
    }
}

void OnlineRelay::SendStatus(const Member& to, const std::string& id, uint8_t code, uint8_t players, uint32_t echo)
{
    uint8_t m[HeaderSize + 6];
    std::memcpy(m, "WMOL", 4);
    m[4] = Version;
    m[5] = Status;
    std::memcpy(m + 6, id.data(), 16);
    m[HeaderSize] = code;
    m[HeaderSize + 1] = players;
    for (int i = 0; i < 4; i++) m[HeaderSize + 2 + i] = (uint8_t)(echo >> (8 * i));
    sendto(Sock, (const char*)m, sizeof(m), 0, (const sockaddr*)to.addr, sizeof(sockaddr_in));
}

void OnlineRelay::Handle(const uint8_t* msg, int len, const void* from)
{
    if (len < (int)HeaderSize || std::memcmp(msg, "WMOL", 4) != 0 || msg[4] != Version) return;
    const uint8_t type = msg[5];
    const std::string id((const char*)msg + 6, 16);
    const uint8_t* body = msg + HeaderSize;
    const int bodyLen = len - (int)HeaderSize;
    const double now = RelayNowMs();

    if (type == Hello && bodyLen >= 9)
    {
        const bool guest = body[0] == 1;
        const uint32_t session = body[1] | body[2] << 8 | body[3] << 16 | (uint32_t)body[4] << 24;
        const uint32_t echo = body[5] | body[6] << 8 | body[7] << 16 | (uint32_t)body[8] << 24;
        auto it = Rooms.find(id);
        if (!guest)
        {
            if (it == Rooms.end())
            {
                if (Rooms.size() >= 4096) return;   // (a flood: no new rooms)
                it = Rooms.emplace(id, Room()).first;
            }
            Room& r = it->second;
            Member probe; std::memcpy(probe.addr, from, sizeof(sockaddr_in));
            if (r.host.present && r.host.session != session) { SendStatus(probe, id, RoomInUse, 2, echo); return; }
            const bool fresh = !r.host.present;
            std::memcpy(r.host.addr, from, sizeof(sockaddr_in));
            r.host.session = session;
            r.host.lastSeen = now;
            r.host.present = true;
            if (fresh) Say("room server: a host opened a room (" + AddrText(from) + ")");
            SendStatus(r.host, id, r.guest.present ? Together : Waiting, r.guest.present ? 2 : 1, echo);
        }
        else
        {
            Member probe; std::memcpy(probe.addr, from, sizeof(sockaddr_in));
            if (it == Rooms.end() || !it->second.host.present) { SendStatus(probe, id, NoRoom, 0, echo); return; }
            Room& r = it->second;
            if (r.guest.present && r.guest.session != session) { SendStatus(probe, id, RoomInUse, 2, echo); return; }
            const bool fresh = !r.guest.present;
            std::memcpy(r.guest.addr, from, sizeof(sockaddr_in));
            r.guest.session = session;
            r.guest.lastSeen = now;
            r.guest.present = true;
            SendStatus(r.guest, id, Together, 2, echo);
            if (fresh)
            {
                Say("room server: a guest joined a room (" + AddrText(from) + ")");
                SendStatus(r.host, id, Together, 2, 0);
            }
        }
        return;
    }

    auto it = Rooms.find(id);
    if (it == Rooms.end()) return;
    Room& r = it->second;
    Member* me = r.host.present && SameAddr(r.host.addr, from) ? &r.host : r.guest.present && SameAddr(r.guest.addr, from) ? &r.guest : nullptr;
    if (!me) return;
    Member* other = me == &r.host ? &r.guest : &r.host;
    me->lastSeen = now;
    if (type == Data)
    {
        if (other->present) sendto(Sock, (const char*)msg, len, 0, (const sockaddr*)other->addr, sizeof(sockaddr_in));
    }
    else if (type == Bye)
    {
        me->present = false;
        if (me == &r.host)
        {
            // the room closes with its host
            if (other->present) SendStatus(*other, id, NoRoom, 0, 0);
            Rooms.erase(it);
            Say("room server: a host closed a room");
        }
        else
        {
            SendStatus(r.host, id, Waiting, 1, 0);
            Say("room server: a guest left a room");
        }
    }
}

void OnlineRelay::Expire(double now)
{
    for (auto it = Rooms.begin(); it != Rooms.end();)
    {
        Room& r = it->second;
        if (r.guest.present && now - r.guest.lastSeen > 8000)
        {
            r.guest.present = false;
            if (r.host.present) SendStatus(r.host, it->first, Waiting, 1, 0);
            Say("room server: a guest timed out");
        }
        if (r.host.present && now - r.host.lastSeen > 8000)
        {
            r.host.present = false;
            if (r.guest.present) SendStatus(r.guest, it->first, NoRoom, 0, 0);
            r.guest.present = false;
            Say("room server: a host timed out");
        }
        if (!r.host.present && !r.guest.present) it = Rooms.erase(it);
        else ++it;
    }
}
