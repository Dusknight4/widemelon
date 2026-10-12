/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "SimMP.h"
#include "Platform.h"

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

static const auto SimStart = std::chrono::steady_clock::now();

bool SimMP::Wanted()
{
    return std::getenv("WIDEMELON_MP_DELAY_MS") || std::getenv("WIDEMELON_MP_NOWAIT")
        || std::getenv("WIDEMELON_MP_CLIENT_WAIT_MS") || std::getenv("WIDEMELON_MP_HOST_WAIT_MS");
}

SimMP::SimMP() noexcept
{
    if (const char* d = std::getenv("WIDEMELON_MP_DELAY_MS")) Delay = std::max(0.0, std::atof(d));
    if (const char* j = std::getenv("WIDEMELON_MP_JITTER_MS")) Jitter = std::max(0.0, std::atof(j));
    if (const char* c = std::getenv("WIDEMELON_MP_CLIENT_WAIT_MS")) ClientCap = std::max(0.0, std::atof(c));
    if (const char* h = std::getenv("WIDEMELON_MP_HOST_WAIT_MS")) HostCap = std::max(0.0, std::atof(h));
    if (const char* w = std::getenv("WIDEMELON_MP_NOWAIT"))
    {
        const std::string s = w;
        if (s == "all") NoWait = 0xFFFF;
        else
            for (size_t i = 0; i < s.size(); i++)
                if (s[i] >= '1' && s[i] <= '9') NoWait |= 1 << (s[i] - '1');
    }
    Log(LogLevel::Info, "WideMelon: simulated network for local multiplayer: delay %.1f ms, jitter %.1f ms, not waiting %04X\n",
        Delay, Jitter, NoWait, ClientCap, HostCap);
}

double SimMP::NowMs() const
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - SimStart).count();
}

void SimMP::Process()
{
    // every 5 seconds: how many of each host's exchanges got their replies in time
    std::lock_guard l(Lock);
    const double now = NowMs();
    if (now - LastReport < 5000) return;
    LastReport = now;
    std::string line;
    for (int i = 0; i < kMaxInst; i++)
    {
        if (!Exchanges[i]) continue;
        char part[96];
        std::snprintf(part, sizeof(part), "  instance %d: %u of %u exchanges answered (%.0f%%)", i + 1, Answered[i], Exchanges[i],
                      100.0 * Answered[i] / Exchanges[i]);
        line += part;
        Exchanges[i] = Answered[i] = 0;
    }
    if (line.empty()) return;
    Log(LogLevel::Info, "WideMelon MP test:%s\n", line.c_str());
    if (FILE* f = std::fopen("WideMelon MP test log.txt", "a"))
    {
        std::fprintf(f, "[%.0f s, delay %.1f ms, jitter %.1f ms]%s\n", now / 1000.0, Delay, Jitter, line.c_str());
        std::fclose(f);
    }
}

void SimMP::Begin(int inst)
{
    std::lock_guard l(Lock);
    Connected |= 1 << inst;
    Queue[inst][0].clear();
    Queue[inst][1].clear();
}

void SimMP::End(int inst)
{
    std::lock_guard l(Lock);
    Connected &= ~(1 << inst);
    if (Host == inst) Host = -1;
}

void SimMP::Send(int from, bool reply, u16 aid, const u8* data, int len, u64 timestamp)
{
    std::lock_guard l(Lock);
    const double due = NowMs() + Delay + (Jitter > 0 ? std::uniform_real_distribution<double>(0, Jitter)(Rng) : 0);
    for (int to = 0; to < kMaxInst; to++)
    {
        if (to == from || !(Connected & (1 << to))) continue;
        if (reply && to != Host) continue;
        auto& q = Queue[to][reply ? 1 : 0];
        Packet p;
        p.sender = (u16)from;
        p.aid = aid;
        p.timestamp = timestamp;
        // one network path: a packet never overtakes an earlier one
        p.due = q.empty() ? due : std::max(due, q.back().due);
        if (data && len) p.data.assign(data, data + len);
        q.push_back(std::move(p));
        if (q.size() > 512) q.pop_front();
    }
    Arrived.notify_all();
}

bool SimMP::Take(int inst, int fifo, Packet& out, double timeoutMs)
{
    std::unique_lock l(Lock);
    const double until = NowMs() + timeoutMs;
    for (;;)
    {
        auto& q = Queue[inst][fifo];
        const double now = NowMs();
        if (!q.empty() && q.front().due <= now)
        {
            out = std::move(q.front());
            q.pop_front();
            return true;
        }
        if (now >= until) return false;
        double wake = until;
        if (!q.empty()) wake = std::min(wake, q.front().due);
        Arrived.wait_for(l, std::chrono::duration<double, std::milli>(std::max(0.05, wake - now)));
    }
}

int SimMP::SendPacket(int inst, u8* data, int len, u64 timestamp) { Send(inst, false, 0, data, len, timestamp); return len; }
int SimMP::SendAck(int inst, u8* data, int len, u64 timestamp) { Send(inst, false, 0, data, len, timestamp); return len; }
int SimMP::SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid) { Send(inst, true, aid, data, len, timestamp); return len; }

int SimMP::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    {
        std::lock_guard l(Lock);
        Host = inst;
    }
    Send(inst, false, 0, data, len, timestamp);
    return len;
}

int SimMP::RecvPacket(int inst, u8* data, u64* timestamp)
{
    Packet p;
    if (!Take(inst, 0, p, 0)) return 0;
    if (!p.data.empty()) std::memcpy(data, p.data.data(), p.data.size());
    if (timestamp) *timestamp = p.timestamp;
    return (int)p.data.size();
}

int SimMP::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    // a client waiting for the host's next CMD: the normal local multiplayer
    // waits for it; a real DS can't, so it takes only what has already come
    {
        std::lock_guard l(Lock);
        if (Host >= 0 && !(Connected & (1 << Host))) return -1;
    }
    Packet p;
    if (!Take(inst, 0, p, ClientWait(inst))) return 0;
    if (!p.data.empty()) std::memcpy(data, p.data.data(), p.data.size());
    if (timestamp) *timestamp = p.timestamp;
    return (int)p.data.size();
}

u16 SimMP::RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask)
{
    // the host collecting its clients' replies to the CMD it just sent
    const double wait = HostWait(inst);
    const bool waits = wait > 0;
    u16 got = 0;
    const double until = NowMs() + wait;
    for (;;)
    {
        Packet p;
        if (!Take(inst, 1, p, std::max(0.0, until - NowMs()))) break;
        // waiting, a reply to an earlier CMD is stale, as normally; not waiting,
        // whatever has come in since is the latest word from that client
        if (waits && p.timestamp < timestamp - 32) continue;
        if (p.aid >= 1 && p.aid <= 15 && !p.data.empty())
        {
            std::memcpy(&data[(p.aid - 1) * 1024], p.data.data(), p.data.size());
            got |= 1 << p.aid;
        }
        if (waits && (got & aidmask) == aidmask) break;
        if (!waits)
        {
            std::lock_guard l(Lock);
            if (Queue[inst][1].empty() || Queue[inst][1].front().due > NowMs()) break;
        }
    }
    std::lock_guard l(Lock);
    Exchanges[inst]++;
    if ((got & aidmask) == aidmask) Answered[inst]++;
    return got;
}

}
