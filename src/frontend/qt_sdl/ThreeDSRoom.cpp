// MultiMelon: Azahar's multiplayer rooms for 3DS games (see ThreeDSRoom.h).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>
#include <random>
#include <regex>

#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"
#include "network/verify_user.h"

#include "ThreeDSRoom.h"

namespace ThreeDSRoom
{
namespace
{
std::mutex g_lock;
bool g_init = false;
EventFn g_event;
std::string g_error;            // the last join's failure, or why the room ended
std::string g_address;
uint16_t g_port = 0;
bool g_hosting = false;
bool g_tried = false;           // a join was started (and not left since)
// Azahar's console id hash is SHA-256 of the 3DS's console id, which needs a running game; rooms are joined
// before one starts. A room only refuses two members with the same hash, so a random one per run does.
std::string g_consoleId;

Network::RoomMember::CallbackHandle<Network::RoomMember::State> g_onState;
Network::RoomMember::CallbackHandle<Network::RoomMember::Error> g_onError;
Network::RoomMember::CallbackHandle<Network::ChatEntry> g_onChat;
Network::RoomMember::CallbackHandle<Network::StatusMessageEntry> g_onStatus;

void Fire(int kind, const std::string& text)
{
    EventFn fn;
    {
        std::lock_guard l(g_lock);
        fn = g_event;
    }
    if (fn) fn(kind, text);
}

std::string ErrorText(Network::RoomMember::Error e)
{
    using E = Network::RoomMember::Error;
    switch (e)
    {
    case E::LostConnection: return "The connection to the room was lost.";
    case E::HostKicked: return "The room's host removed you from the room.";
    case E::HostBanned: return "The room's host has banned you from this room.";
    case E::NameCollision: return "Someone in the room already uses that nickname. Pick another.";
    case E::MacCollision: return "Someone in the room already uses this console's address. Try again.";
    case E::ConsoleIdCollision: return "Someone in the room already uses this console. Try again.";
    case E::WrongVersion: return "The room runs a different version of the room software (Azahar or MultiMelon).";
    case E::WrongPassword: return "Wrong password for this room.";
    case E::CouldNotConnect:
        return "The room didn't answer. Check the address and port; a room elsewhere needs its UDP port forwarded "
               "to the host's PC.";
    case E::RoomIsFull: return "The room is full.";
    default: return "The room connection failed.";
    }
}
} // namespace

bool ValidName(const std::string& name)
{
    static const std::regex pattern("^[ a-zA-Z0-9._-]{4,20}$");
    return std::regex_match(name, pattern);
}

bool Init()
{
    std::lock_guard l(g_lock);
    if (g_init) return true;
    if (!Network::Init()) return false;
    g_init = true;

    std::random_device rd;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 64; i++) g_consoleId += hex[rd() & 15];

    if (auto member = Network::GetRoomMember().lock())
    {
        g_onState = member->BindOnStateChanged([](const Network::RoomMember::State& s) {
            using S = Network::RoomMember::State;
            if (s == S::Joined || s == S::Moderator)
            {
                std::lock_guard l(g_lock);
                g_error.clear();
            }
            Fire(0, "");
        });
        g_onError = member->BindOnError([](const Network::RoomMember::Error& e) {
            const std::string text = ErrorText(e);
            {
                std::lock_guard l(g_lock);
                g_error = text;
            }
            Fire(0, text);
        });
        g_onChat = member->BindOnChatMessageRecieved(
            [](const Network::ChatEntry& c) { Fire(1, c.nickname + ": " + c.message); });
        g_onStatus = member->BindOnStatusMessageReceived([](const Network::StatusMessageEntry& m) {
            const char* what = nullptr;
            switch (m.type)
            {
            case Network::IdMemberJoin: what = " joined the room"; break;
            case Network::IdMemberLeave: what = " left the room"; break;
            case Network::IdMemberKicked: what = " was removed from the room"; break;
            case Network::IdMemberBanned: what = " was banned from the room"; break;
            default: return;
            }
            Fire(2, m.nickname + what);
        });
    }
    return true;
}

void Shutdown()
{
    if (!g_init) return;
    Leave();
    // (not under g_lock: the room member calls our callbacks holding its own lock, and they take g_lock)
    if (auto member = Network::GetRoomMember().lock())
    {
        member->Unbind(g_onState);
        member->Unbind(g_onError);
        member->Unbind(g_onChat);
        member->Unbind(g_onStatus);
    }
    Network::Shutdown();
    std::lock_guard l(g_lock);
    g_init = false;
}

std::string Host(const std::string& roomName, const std::string& nickname, uint16_t port, int capacity,
                 const std::string& password, const std::string& bindAddress, const std::string& game, uint64_t gameId)
{
    if (!Init()) return "The room network couldn't start.";
    Leave();
    auto room = Network::GetRoom().lock();
    if (!room) return "The room network couldn't start.";
    if (!room->Create(roomName, "", bindAddress, port, password, (uint32_t)std::max(capacity, 2), nickname, game, gameId,
                      std::make_unique<Network::VerifyUser::NullBackend>()))
        return "The room couldn't open on UDP port " + std::to_string(port) +
               ". Is another program (or another MultiMelon) using it?";
    {
        std::lock_guard l(g_lock);
        g_hosting = true;
    }
    Join(bindAddress.empty() ? "127.0.0.1" : bindAddress, port, nickname, password);
    return "";
}

void Join(const std::string& address, uint16_t port, const std::string& nickname, const std::string& password)
{
    if (!Init()) return;
    auto member = Network::GetRoomMember().lock();
    if (!member) return;
    if (member->IsConnected()) member->Leave();
    std::string id;
    {
        std::lock_guard l(g_lock);
        g_address = address;
        g_port = port;
        g_error.clear();
        g_tried = true;
        id = g_consoleId;
    }
    member->Join(nickname, id, address.c_str(), port, 0, Network::NoPreferredMac, password);
}

void Leave()
{
    if (!g_init) return;
    if (auto member = Network::GetRoomMember().lock())
    {
        using S = Network::RoomMember::State;
        const S s = member->GetState();
        if (s == S::Joining || s == S::Joined || s == S::Moderator) member->Leave();
    }
    if (auto room = Network::GetRoom().lock(); room && room->GetState() == Network::Room::State::Open) room->Destroy();
    std::lock_guard l(g_lock);
    g_hosting = false;
    g_tried = false;
    g_error.clear();
}

Info GetInfo()
{
    Info i;
    if (!g_init) return i;
    auto member = Network::GetRoomMember().lock();
    if (!member) return i;
    using S = Network::RoomMember::State;
    const S s = member->GetState();
    {
        std::lock_guard l(g_lock);
        i.error = g_error;
        i.address = g_address;
        i.port = g_port;
        if (s == S::Joining) i.state = State::Joining;
        else if (s == S::Joined || s == S::Moderator) i.state = State::Joined;
        else i.state = (g_tried && !g_error.empty()) ? State::Failed : State::Idle;
    }
    if (auto room = Network::GetRoom().lock()) i.hosting = room->GetState() == Network::Room::State::Open;
    if (i.state == State::Joined)
    {
        const Network::RoomInformation info = member->GetRoomInformation();
        i.roomName = info.name;
        i.capacity = (int)info.member_slots;
        if (info.port) i.port = info.port;
        const Network::RoomMember::MemberList list = member->GetMemberInformation();
        for (const auto& m : list) i.members.push_back({m.nickname, m.game_info.name});
    }
    return i;
}

bool InRoom()
{
    if (!g_init) return false;
    auto member = Network::GetRoomMember().lock();
    if (!member) return false;
    using S = Network::RoomMember::State;
    const S s = member->GetState();
    return s == S::Joining || s == S::Joined || s == S::Moderator;
}

void SendChat(const std::string& text)
{
    if (!g_init || text.empty()) return;
    if (auto member = Network::GetRoomMember().lock(); member && member->IsConnected()) member->SendChatMessage(text);
}

void SetEventHandler(EventFn fn)
{
    std::lock_guard l(g_lock);
    g_event = std::move(fn);
}

} // namespace ThreeDSRoom
