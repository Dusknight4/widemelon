/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef SIMMP_H
#define SIMMP_H

#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <vector>
#include "types.h"
#include "MPInterface.h"

namespace melonDS
{

// WideMelon: local multiplayer between the instances of this app, over a
// simulated network, to try out a real DS on WiFi as a partner: packets arrive
// late (WIDEMELON_MP_DELAY_MS, plus up to WIDEMELON_MP_JITTER_MS at random),
// and the instances listed in WIDEMELON_MP_NOWAIT ("all", or numbers from 1
// like "1,2") never wait for their partners, as a real DS can't. Late replies
// then count as whatever the partner said last. WIDEMELON_MP_CLIENT_WAIT_MS and
// WIDEMELON_MP_HOST_WAIT_MS instead cap, for every instance, how long a client
// waits for the host's next CMD and a host for its replies. Used in place of the normal
// local multiplayer when either setting is there.
class SimMP : public MPInterface
{
public:
    SimMP() noexcept;
    static bool Wanted();

    void Process() override;

    void Begin(int inst) override;
    void End(int inst) override;

    int SendPacket(int inst, u8* data, int len, u64 timestamp) override;
    int RecvPacket(int inst, u8* data, u64* timestamp) override;
    int SendCmd(int inst, u8* data, int len, u64 timestamp) override;
    int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid) override;
    int SendAck(int inst, u8* data, int len, u64 timestamp) override;
    int RecvHostPacket(int inst, u8* data, u64* timestamp) override;
    u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask) override;

private:
    static constexpr int kMaxInst = 16;
    struct Packet
    {
        u16 sender, aid;
        u64 timestamp;
        double due;
        std::vector<u8> data;
    };

    double NowMs() const;
    void Send(int from, bool reply, u16 aid, const u8* data, int len, u64 timestamp);
    bool Take(int inst, int fifo, Packet& out, double timeoutMs);
    // how long a client waits for the host's next CMD, and a host for its clients' replies
    double ClientWait(int inst) const { return ClientCap >= 0 ? ClientCap : (NoWait & (1 << inst)) ? 0 : RecvTimeout; }
    double HostWait(int inst) const { return HostCap >= 0 ? HostCap : (NoWait & (1 << inst)) ? 0 : RecvTimeout; }

    std::mutex Lock;
    std::condition_variable Arrived;
    std::deque<Packet> Queue[kMaxInst][2];  // [to instance][0 frames, 1 replies]
    u16 Connected = 0;
    int Host = -1;
    u16 NoWait = 0;
    double ClientCap = -1, HostCap = -1;    // ms, for every instance; -1: by NoWait
    double Delay = 0, Jitter = 0;
    std::mt19937 Rng {12345};
    u32 Exchanges[kMaxInst] {}, Answered[kMaxInst] {};
    double LastReport = 0;
};

}

#endif // SIMMP_H
