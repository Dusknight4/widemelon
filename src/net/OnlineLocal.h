/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef WIDEMELON_ONLINELOCAL_H
#define WIDEMELON_ONLINELOCAL_H

// "Online as Local": a game's local wireless (Union Room trades and battles...) with a
// friend's WideMelon over the internet. The game's wireless requests don't go to the
// emulated ARM7 wireless but to wmhle (the same WM HLE the DS WiFi bridge runs on a real
// DS), and what it would put on the air goes, as DsBridge messages, through a room
// server (OnlineRelay) to the friend's wmhle. Unlike emulated local wireless, neither
// side ever waits for the other: a round trip of a few hundred ms only makes the link
// itself slower (a trade takes a little longer), never the game.
//
// Both players enter the same room name and password; one hosts (opens the room), the
// other joins. Turn it on before entering the Union Room (or another wireless menu).

#include <cstdint>
#include <string>

namespace melonDS { class NDS; }

namespace OnlineLocal
{
enum class State { Off, Connecting, NoServer, Waiting, Together, NoRoom, RoomInUse, PortTaken };

struct Settings
{
    std::string Server;     // room server, "host" or "host:port" (default port 7300)
    bool Host = true;       // opens the room (else joins it)
    uint8_t RoomId[16] {};  // RoomIdFrom(name, password)
    bool RunServer = false; // also run the room server in this WideMelon (on the port of Server)
};

// a room's id: SHA-256 of the name and password (in that order, each length-prefixed), cut to 16 bytes
void RoomIdFrom(const std::string& name, const std::string& password, uint8_t out[16]);

bool Start(const Settings& s);
void Stop();
bool Enabled();
State GetState();
int PingMs();               // round trip to the room server (-1 not known)
std::string StatusText();   // for the menu/dialog
// (testing) also hand each line of wmhle's log to this
void SetLogHook(void (*hook)(const char*));

// the emulator thread, before each frame of the first instance: puts wmhle in the
// console's place for its wireless while on (again for a new console), takes it out when off
void Frame(melonDS::NDS* nds);
}

#endif
