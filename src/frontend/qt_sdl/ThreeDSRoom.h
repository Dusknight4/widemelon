// MultiMelon: Azahar's multiplayer rooms for 3DS games ("Create Room" / "Direct Connect", as in Azahar).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A room is a small server one player runs (UDP, port 24872 by default) that every player connects to,
// over the internet or a LAN. While this MultiMelon is in a room, the 3DS game's local wireless goes
// through it (Azahar's own room protocol, so Azahar players can be in the same room) instead of
// "Wifi as Local Multiplayer". No Qt here: the rig uses it too.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ThreeDSRoom
{

constexpr uint16_t DefaultPort = 24872;

enum class State { Idle, Joining, Joined, Failed };

struct Member
{
    std::string nickname;
    std::string game;       // the game they're playing ("" none)
};

struct Info
{
    State state = State::Idle;
    std::string error;      // why it failed or ended (Failed, or Idle after a lost room)
    bool hosting = false;   // this MultiMelon runs the room
    std::string roomName;
    std::string address;    // what was joined ("127.0.0.1" when hosting)
    uint16_t port = 0;
    int capacity = 0;       // most members the room takes
    std::vector<Member> members;
};

// Azahar's network (rooms). Before a 3DS game starts, so its local wireless can use a room; again: nothing.
bool Init();
void Shutdown();

// valid nickname / room name: 4 to 20 letters, digits, spaces, '.', '_', '-' (what rooms accept)
bool ValidName(const std::string& name);

// Opens a room on this PC and joins it. bindAddress "" takes every network (for friends elsewhere: their
// router must forward the UDP port to this PC); "127.0.0.1" this PC only (tests). "" when it opened, else why not.
std::string Host(const std::string& roomName, const std::string& nickname, uint16_t port, int capacity,
                 const std::string& password, const std::string& bindAddress = "", const std::string& game = "",
                 uint64_t gameId = 0);
// Joins a room at address (a name or an IP) and port. It goes on in the background: GetInfo says how.
void Join(const std::string& address, uint16_t port, const std::string& nickname, const std::string& password);
// Leaves the room (and closes it if this MultiMelon runs it).
void Leave();

Info GetInfo();
// joining or joined: the 3DS game's local wireless goes to the room
bool InRoom();
void SendChat(const std::string& text);

// What happens, called on the network's own thread: kind 0 the state changed (text: the error if any),
// 1 a chat line ("nickname: text"), 2 someone joined/left ("Summer joined the room").
using EventFn = std::function<void(int kind, const std::string& text)>;
void SetEventHandler(EventFn fn);

}
