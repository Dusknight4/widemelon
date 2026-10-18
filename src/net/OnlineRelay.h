/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef WIDEMELON_ONLINERELAY_H
#define WIDEMELON_ONLINERELAY_H

// "Online as Local": the room server. Two WideMelons that want to play together over
// the internet each send to it (so neither needs its router set up), and it passes
// each one's packets on to the other. A room is known only by a 16-byte id, a hash of
// its name and password made by the players (the server never sees either), and holds
// a host and one guest.
//
// Messages (UDP, one per datagram): "WMOL", version (1), type, the 16-byte room id,
// then by type:
//   Hello  (player -> server, every second while connected; also the keepalive):
//          u8 role (0 host, 1 guest), u32 session (random, one per connect), u32 time (echoed)
//   Status (server -> player, the answer to each Hello, and when the other comes or goes):
//          u8 code (OnlineStatus), u8 players in the room, u32 the Hello's time (0 if none)
//   Data   (player -> server -> the other player): the payload (a DsBridge message)
//   Bye    (player -> server): leaving
// All numbers little-endian. A player silent for 8 seconds is dropped from its room.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace OnlineProto
{
constexpr uint8_t Version = 1;
enum Type : uint8_t { Hello = 1, Status = 2, Data = 3, Bye = 4 };
enum Code : uint8_t { Waiting = 0, Together = 1, NoRoom = 2, RoomInUse = 3 };
constexpr size_t HeaderSize = 4 + 1 + 1 + 16;
constexpr uint16_t DefaultPort = 7300;
}

class OnlineRelay
{
public:
    using LogFn = std::function<void(const std::string&)>;

    OnlineRelay() = default;
    ~OnlineRelay() { Stop(); }

    // listens on the UDP port (all interfaces) on a thread of its own; false if the port is taken
    bool Start(uint16_t port, LogFn log = nullptr);
    void Stop();
    bool Running() const { return Thread.joinable(); }

    // runs on this thread until Stop (the standalone server)
    bool RunHere(uint16_t port, LogFn log = nullptr);

private:
    struct Member { uint8_t addr[16] {}; uint32_t session = 0; double lastSeen = 0; bool present = false; };
    struct Room { Member host, guest; };

    bool Open(uint16_t port);
    void Loop();
    void Handle(const uint8_t* msg, int len, const void* from);
    void SendStatus(const Member& to, const std::string& id, uint8_t code, uint8_t players, uint32_t echo);
    void Expire(double now);
    void Say(const std::string& s) { if (Log) Log(s); }

    uintptr_t Sock = ~(uintptr_t)0;
    std::atomic<bool> StopFlag {false};
    std::thread Thread;
    LogFn Log;
    std::map<std::string, Room> Rooms;
    double LastExpire = 0;
};

#endif
