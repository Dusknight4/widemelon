/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef BRIDGEMP_H
#define BRIDGEMP_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "types.h"
#include "MPInterface.h"
#include "DsBridge.h"

namespace melonDS
{

// WideMelon: local multiplayer with one remote DS over UDP, through the DS
// bridge (see DsBridge.h): a real DS running the game under the modified
// nds-bootstrap, or another WideMelon running this too. One instance of this
// app takes part (the first that turns its wireless on).
//
// Timing, as tested over simulated WiFi: when the local DS hosts, it waits for
// the remote's reply to each CMD (up to HostWaitMs); as a client it never waits for the host's next CMD, so the
// emulator never stalls on a slow link.
//
// Set up through the environment:
//   WIDEMELON_BRIDGE              "listen", or the remote's address: "ip" or "ip:port"
//   WIDEMELON_BRIDGE_PORT         local UDP port (default 7071; also the remote's default)
//   WIDEMELON_BRIDGE_HOST_WAIT_MS how long the local host waits for a reply (default 25)
//   WIDEMELON_BRIDGE_HOST_FIT_FRAME "1": wait only within the frame's spare time (an experiment,
//                                 off: on a real link it lost most replies, ~20 FPS, no talks)
//   WIDEMELON_BRIDGE_CLIENT_WAIT_MS how long the local client waits for a CMD (default 0)
//   WIDEMELON_BRIDGE_LOCAL_ACK    "0": with a real DS host, wait for its acks instead of
//                                 acking our replies ourselves (default on)
//   WIDEMELON_BRIDGE_TRUST_CLOCK "1": take the remote's clock and melonDS header as
//                                 they come (two WideMelons; default off, as for a real DS)
//   WIDEMELON_BRIDGE_TEST_DELAY_MS, _TEST_JITTER_MS: hold what comes in that long, to
//                                 try the link as if over WiFi
class BridgeMP : public MPInterface
{
public:
    struct Config
    {
        u16 Port = 7071;
        std::string Peer;           // "ip" or "ip:port"; empty: listen, and answer whoever writes
        double HostWaitMs = 25;   // (16 with HostFitFrame was tried, exe v11: unusable on a real link)
        // as host: wait only while the emulated clock is ahead of real time (time the frame
        // limiter would sleep anyway), so waiting never costs a frame; at least HostMinWaitMs
        // (an emulator running behind still hears a nearby DS). A reply that comes later is
        // dropped as before, and both ends send its data again (the WM ports' resending)
        bool HostFitFrame = false;
        double HostMinWaitMs = 4;
        double ClientWaitMs = 0;
        double AckWaitMs = 16;    // as a client: for the host's ack after our reply (once a round)
        bool PaceToDs = true;     // as a real DS's client: take its CMDs at about the real time they came (see RecvHostPacket)
        double PaceMaxMs = 16;    // at most this long a wait for that
        double PaceBudgetMs = 12; // and at most this much waiting per 1/60 s of real time
        bool LocalAck = true;     // as a real DS's client: ack our own replies at once (see SendReply)
        u64 CmdGapUs = 10000;     // a CMD comes at least this long (emulated) after the ack before it (see Hand, SendReply)
        bool TrustClock = false;
        double TestDelayMs = 0, TestJitterMs = 0;
    };

    static bool Wanted();
    static void SetUserEnabled(bool on);    // the menu option (with no environment set: listen on 7071)
    static Config FromEnv();

    explicit BridgeMP(const Config& config) noexcept;
    ~BridgeMP() override;

    bool Ok() const { return Sock != kNoSocket; }

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

    // for the test harness: counts since the start
    struct Counts { u32 MsgsOut = 0, MsgsIn = 0, Exchanges = 0, Answered = 0, StaleReplies = 0, Replies = 0, NotReady = 0, NotReadyRun = 0; double WaitMs = 0, WaitMaxMs = 0; };   // (client: replies sent, how many "not ready", the longest run of those) // stale: answered an earlier CMD
    Counts GetCounts();

private:
#ifdef _WIN32
    using Socket = uintptr_t;
#else
    using Socket = int;
#endif
    static constexpr Socket kNoSocket = (Socket)~(Socket)0;

    struct Delayed { DsBridge::Msg M; double Due; };

    double NowMs() const;
    bool Mine(int inst);
    void Send(int inst, u32 mpType, u16 aid, const u8* data, int len, u64 timestamp);
    void SendMsg(DsBridge::Msg& m);         // lock held
    void FromRemote(const DsBridge::Msg& m); // lock held
    void PumpDelayed();                     // lock held
    bool Take(int fifo, DsBridge::Frame& out, double timeoutMs);
    bool NewerCmdQueued();                  // a CMD waiting behind what was just taken
    bool RemoteIsDs = false;                // the remote is a real DS (its Hello says "DSHW"): for the log
    u64 EmuNowUs = 0, EmuAnchorUs = 0;      // the emulated clock, from the frames we send (emulation thread only)
    double EmuNowAt = 0, RealAnchorMs = 0;
    double PaceWindowStart = 0, PaceSpent = 0;
    double EmuAheadMs();
    bool AckMissed = false;                 // we stopped waiting for the last ack: if it comes now, it's dropped
    bool AwaitingAck = false;               // we replied: the host's ack is next (emulation thread only)
    bool SkippedCmd = false;                // the last CMD was skipped (a newer one behind it): its ack goes too
    DsBridge::Msg AckTemplate;              // the DS host's last ack: our local acks are made from it (lock held)
    bool HaveAckTemplate = false;
    u16 LocalAckCounter = 0;
    u64 LastAckUs = 0;                      // the last ack's time as handed to the local DS (emulation thread only)
    u32 NotReadyRun = 0;                    // "not ready" replies in a row (for the log)
    u16 UsedReplyRef = 0xFFFF;              // as host: the CMD the remote's reply we took this round answers (lock held)
    int Hand(const DsBridge::Frame& f, u8* data, u64* timestamp);  // to the local DS
    void RecvLoop();

    Config Cfg;
    Socket Sock = kNoSocket;
    u8 PeerAddr[16] {};                     // a sockaddr_in
    bool PeerKnown = false;
    std::thread Receiver;
    std::atomic<bool> Stop {false};

    std::mutex Lock;
    std::condition_variable Arrived;
    DsBridge::Proxy Proxy;
    std::deque<DsBridge::Frame> Queue[2];   // for the local DS: [0 frames, 1 replies]
    std::deque<Delayed> Incoming;           // held back by the test delay
    std::mt19937 Rng {12345};
    int Owner = -1;                         // the instance taking part
    u16 CurCmdSeq = 0xFFFF;                 // local host: the CMD it's collecting replies for
    u16 LastCmdSeq = 0xFFFF;                // local client: the last CMD handed to it
    double LastHello = -1e9, LastReport = 0, LastHeard = -1e9;
    Counts Total, Window;

    // the last few seconds of messages both ways, with their contents (the games' data in
    // each CMD/reply): written to "WideMelon link capture.txt" whenever a link ends, to see
    // exactly what a game had last before it gave up (lock held)
    struct CapEntry { double T; bool Out; u8 Type; u16 Seq, Ref, Aid; std::vector<u8> Body; };
    std::deque<CapEntry> Capture;
    void CaptureMsg(bool out, const DsBridge::Msg& m);
    void DumpCapture(const char* why);
};

}

#endif // BRIDGEMP_H
