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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "BridgeMP.h"
#include "Platform.h"

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

static const auto BridgeStart = std::chrono::steady_clock::now();

static bool s_userEnabled = false;

void BridgeMP::SetUserEnabled(bool on) { s_userEnabled = on; }

bool BridgeMP::Wanted()
{
    // the Multiplayer menu's "Wifi as Local Multiplayer" (listening on the default
    // port), or the environment (the test harness, older launchers)
    const char* b = std::getenv("WIDEMELON_BRIDGE");
    return s_userEnabled || (b && b[0]);
}

BridgeMP::Config BridgeMP::FromEnv()
{
    Config c;
    if (const char* b = std::getenv("WIDEMELON_BRIDGE"))
        if (std::strcmp(b, "listen") != 0) c.Peer = b;
    if (const char* p = std::getenv("WIDEMELON_BRIDGE_PORT")) c.Port = (u16)std::atoi(p);
    if (const char* h = std::getenv("WIDEMELON_BRIDGE_HOST_WAIT_MS")) c.HostWaitMs = std::max(0.0, std::atof(h));
    if (const char* h = std::getenv("WIDEMELON_BRIDGE_HOST_FIT_FRAME")) c.HostFitFrame = h[0] != '0';
    if (const char* w = std::getenv("WIDEMELON_BRIDGE_CLIENT_WAIT_MS")) c.ClientWaitMs = std::max(0.0, std::atof(w));
    if (const char* a = std::getenv("WIDEMELON_BRIDGE_ACK_WAIT_MS")) c.AckWaitMs = std::max(0.0, std::atof(a));
    if (const char* b = std::getenv("WIDEMELON_BRIDGE_PACE_TO_DS")) c.PaceToDs = b[0] != '0';
    if (const char* k = std::getenv("WIDEMELON_BRIDGE_LOCAL_ACK")) c.LocalAck = k[0] != '0';
    if (const char* g = std::getenv("WIDEMELON_BRIDGE_CMD_GAP_US")) c.CmdGapUs = (u64)std::max(0, std::atoi(g));
    if (const char* t = std::getenv("WIDEMELON_BRIDGE_TRUST_CLOCK")) c.TrustClock = t[0] == '1';
    if (const char* d = std::getenv("WIDEMELON_BRIDGE_TEST_DELAY_MS")) c.TestDelayMs = std::max(0.0, std::atof(d));
    if (const char* j = std::getenv("WIDEMELON_BRIDGE_TEST_JITTER_MS")) c.TestJitterMs = std::max(0.0, std::atof(j));
    return c;
}

BridgeMP::BridgeMP(const Config& config) noexcept : Cfg(config)
{
    Proxy.SetForeignRemote(!Cfg.TrustClock);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Socket s = (Socket)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket)
    {
        Log(LogLevel::Error, "WideMelon bridge: no UDP socket\n");
        return;
    }
    sockaddr_in local {};
    local.sin_family = AF_INET;
    // (WIDEMELON_BRIDGE_LOOPBACK: only this PC can reach it; for tests)
    local.sin_addr.s_addr = htonl(std::getenv("WIDEMELON_BRIDGE_LOOPBACK") ? INADDR_LOOPBACK : INADDR_ANY);
    local.sin_port = htons(Cfg.Port);
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        Log(LogLevel::Error, "WideMelon bridge: UDP port %d is taken\n", Cfg.Port);
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return;
    }
    // the receiver looks at Stop this often
#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv {0, 100000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    Sock = s;

    if (!Cfg.Peer.empty())
    {
        std::string host = Cfg.Peer, port = std::to_string(Cfg.Port);
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos) { port = host.substr(colon + 1); host = host.substr(0, colon); }
        addrinfo hints {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0 && res)
        {
            std::memcpy(PeerAddr, res->ai_addr, sizeof(sockaddr_in));
            PeerKnown = true;
            freeaddrinfo(res);
        }
        else
            Log(LogLevel::Error, "WideMelon bridge: can't find %s\n", Cfg.Peer.c_str());
    }

    Log(LogLevel::Info, "WideMelon bridge: UDP port %d, %s %s; host waits %.0f ms, client %.0f ms%s\n", Cfg.Port,
        PeerKnown ? "remote" : "listening", PeerKnown ? Cfg.Peer.c_str() : "", Cfg.HostWaitMs, Cfg.ClientWaitMs,
        Cfg.TrustClock ? ", remote clock trusted" : "");
    Receiver = std::thread([this] { RecvLoop(); });
}

BridgeMP::~BridgeMP()
{
    Stop = true;
    if (Receiver.joinable()) Receiver.join();
    if (Sock != kNoSocket)
    {
        {
            // tell the remote we're gone
            std::lock_guard l(Lock);
            if (Owner >= 0 && PeerKnown)
            {
                DsBridge::Msg m;
                m.Type = DsBridge::MsgType::Leave;
                SendMsg(m);
            }
        }
#ifdef _WIN32
        closesocket(Sock);
        WSACleanup();
#else
        close(Sock);
#endif
    }
}

double BridgeMP::NowMs() const
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - BridgeStart).count();
}

BridgeMP::Counts BridgeMP::GetCounts()
{
    std::lock_guard l(Lock);
    return Total;
}

void BridgeMP::RecvLoop()
{
    std::vector<u8> buf(4096);
    while (!Stop)
    {
        sockaddr_in from {};
        socklen_t fromLen = sizeof(from);
        const int n = (int)recvfrom(Sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) continue;
        DsBridge::Msg m;
        if (!m.Parse(buf.data(), (size_t)n)) continue;

        std::lock_guard l(Lock);
        CaptureMsg(false, m);
        // listening: answer whoever writes to us
        if (Cfg.Peer.empty())
        {
            if (!PeerKnown || std::memcmp(PeerAddr, &from, sizeof(from)) != 0)
            {
                char ip[64] = "?";
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                Log(LogLevel::Info, "WideMelon bridge: remote is %s:%d\n", ip, ntohs(from.sin_port));
                if (FILE* f = std::fopen("WideMelon bridge log.txt", "a"))
                {
                    std::fprintf(f, "[%.0f s] remote is %s:%d\n", NowMs() / 1000.0, ip, ntohs(from.sin_port));
                    std::fclose(f);
                }
            }
            std::memcpy(PeerAddr, &from, sizeof(from));
            PeerKnown = true;
        }
        LastHeard = NowMs();
        if (m.Type == DsBridge::MsgType::Hello && !RemoteIsDs && m.Body.size() >= 4 && std::memcmp(m.Body.data(), "DSHW", 4) == 0)
        {
            // a real DS (through the modified nds-bootstrap): noted for the log
            RemoteIsDs = true;
            if (FILE* f = std::fopen("WideMelon bridge log.txt", "a"))
            {
                std::fprintf(f, "[%.0f s] the remote is a real DS\n", NowMs() / 1000.0);
                std::fclose(f);
            }
        }
        if (m.Type == DsBridge::MsgType::Log)
        {
            if (FILE* f = std::fopen("WideMelon bridge log.txt", "a"))
            {
                // each line as "[DS] ..."
                std::string text(m.Body.begin(), m.Body.end());
                size_t at = 0;
                while (at < text.size())
                {
                    size_t nl = text.find('\n', at);
                    if (nl == std::string::npos) nl = text.size();
                    std::fprintf(f, "[DS] %s\n", text.substr(at, nl - at).c_str());
                    at = nl + 1;
                }
                std::fclose(f);
            }
            continue;
        }
        Total.MsgsIn++;
        Window.MsgsIn++;
        // a real DS host's acks: kept as the pattern for our own (SendReply), and
        // dropped once we have one (each of our replies is acked here, at once)
        if (RemoteIsDs && Cfg.LocalAck && m.Type == DsBridge::MsgType::Ack)
        {
            const bool first = !HaveAckTemplate;
            AckTemplate = m;
            HaveAckTemplate = true;
            if (!first) continue;
        }
        if (Cfg.TestDelayMs > 0 || Cfg.TestJitterMs > 0)
        {
            double due = NowMs() + Cfg.TestDelayMs
                + (Cfg.TestJitterMs > 0 ? std::uniform_real_distribution<double>(0, Cfg.TestJitterMs)(Rng) : 0);
            // one network path: nothing overtakes what came before it
            if (!Incoming.empty()) due = std::max(due, Incoming.back().Due);
            Incoming.push_back({std::move(m), due});
        }
        else
            FromRemote(m);
        Arrived.notify_all();
    }
}

void BridgeMP::FromRemote(const DsBridge::Msg& m)
{
    // (for the log: the remote leaving or ending the link, and what was still waiting here)
    if (m.Type == DsBridge::MsgType::Leave)
        if (FILE* f = std::fopen("WideMelon bridge log.txt", "a"))
        {
            std::fprintf(f, "[%.1f s] the remote left (or ended the link); here still queued: %zu replies, %zu other frames\n",
                         NowMs() / 1000.0, Queue[1].size(), Queue[0].size());
            std::fclose(f);
            DumpCapture("the remote left");
        }
    std::vector<DsBridge::Frame> frames;
    Proxy.FromRemote(m, frames);
    for (auto& f : frames)
    {
        auto& q = Queue[f.MPType == 2 ? 1 : 0];
        q.push_back(std::move(f));
        if (q.size() > 512) q.pop_front();
    }
}

void BridgeMP::PumpDelayed()
{
    const double now = NowMs();
    while (!Incoming.empty() && Incoming.front().Due <= now)
    {
        FromRemote(Incoming.front().M);
        Incoming.pop_front();
    }
}

void BridgeMP::CaptureMsg(bool out, const DsBridge::Msg& m)
{
    if (m.Type == DsBridge::MsgType::Hello || m.Type == DsBridge::MsgType::Log) return;
    const double now = NowMs();
    Capture.push_back({now, out, (u8)m.Type, m.Seq, m.RefSeq, m.Aid, m.Body});
    while (!Capture.empty() && (now - Capture.front().T > 4000 || Capture.size() > 3000)) Capture.pop_front();
}

void BridgeMP::DumpCapture(const char* why)
{
    FILE* f = std::fopen("WideMelon link capture.txt", "a");
    if (!f) return;
    const double now = NowMs();
    std::fprintf(f, "=== [%.1f s] %s: the last %zu messages (ms before now, out/in, type, seq, ref, aid, body)\n",
                 now / 1000.0, why, Capture.size());
    static const char* names[] = {"?", "info", "join", "joinok", "cmd", "reply", "ack", "leave", "hello", "log"};
    for (const auto& e : Capture)
    {
        std::fprintf(f, "%8.1f %s %-6s %04X %04X %u %3zu ", e.T - now, e.Out ? "out" : "in ",
                     e.Type < 10 ? names[e.Type] : "?", e.Seq, e.Ref, e.Aid, e.Body.size());
        for (u8 b : e.Body) std::fprintf(f, "%02X", b);
        std::fputc('\n', f);
    }
    std::fclose(f);
}

void BridgeMP::SendMsg(DsBridge::Msg& m)
{
    if (!PeerKnown) return;
    CaptureMsg(true, m);
    const std::vector<u8> wire = m.Serialize();
    sendto(Sock, (const char*)wire.data(), (int)wire.size(), 0, (const sockaddr*)PeerAddr, sizeof(sockaddr_in));
    Total.MsgsOut++;
    Window.MsgsOut++;
}

bool BridgeMP::Mine(int inst)
{
    // the first instance to use the wireless takes part (a savestate with the
    // wireless already on never calls Begin)
    std::lock_guard l(Lock);
    if (Owner < 0 && Sock != kNoSocket) Owner = inst;
    return Owner == inst;
}

void BridgeMP::Process()
{
    std::lock_guard l(Lock);
    const double now = NowMs();
    // once a second to whoever is on the other end, so a DS saying hello by
    // broadcast learns where we are, whether or not our game uses its wireless yet
    if (PeerKnown && now - LastHello >= 1000)
    {
        LastHello = now;
        DsBridge::Msg m;
        m.Type = DsBridge::MsgType::Hello;
        SendMsg(m);
    }
    // every 5 seconds: how the link is doing
    if (now - LastReport < 5000) return;
    LastReport = now;
    if (!Window.MsgsIn && !Window.MsgsOut) return;
    char line[400];
    std::snprintf(line, sizeof(line), "messages out %u, in %u; as host %u of %u exchanges answered, %u replies to an earlier CMD (taken anyway), waiting %.1f ms a round (longest %.0f); as client %u replies, %u \"not ready\" (longest run %u)%s",
                  Window.MsgsOut, Window.MsgsIn, Window.Answered, Window.Exchanges, Window.StaleReplies,
                  Window.Exchanges ? Window.WaitMs / Window.Exchanges : 0.0, Window.WaitMaxMs,
                  Window.Replies, Window.NotReady, Window.NotReadyRun,
                  now - LastHeard > 3000 ? "; the remote is silent" : "");
    Log(LogLevel::Info, "WideMelon bridge: %s\n", line);
    if (FILE* f = std::fopen("WideMelon bridge log.txt", "a"))
    {
        std::fprintf(f, "[%.0f s] %s\n", now / 1000.0, line);
        std::fclose(f);
    }
    Window = Counts();
}

void BridgeMP::Begin(int inst)
{
    std::lock_guard l(Lock);
    if (Owner < 0 && Sock != kNoSocket) Owner = inst;
    if (Owner != inst) return;
    Queue[0].clear();
    Queue[1].clear();
}

void BridgeMP::End(int inst)
{
    std::lock_guard l(Lock);
    if (Owner != inst) return;
    DsBridge::Msg m;
    m.Type = DsBridge::MsgType::Leave;
    SendMsg(m);
    Owner = -1;
}

double BridgeMP::EmuAheadMs()
{
    // how far the emulated clock (as of the last frame we sent) is ahead of real time,
    // counted from the moment it was least ahead (re-anchored whenever it falls behind)
    return (double)(EmuNowUs - EmuAnchorUs) / 1000.0 - (NowMs() - RealAnchorMs);
}

void BridgeMP::Send(int inst, u32 mpType, u16 aid, const u8* data, int len, u64 timestamp)
{
    if (timestamp)
    {
        EmuNowUs = timestamp;
        EmuNowAt = NowMs();
        // (re-anchored when it falls behind, and on a jump ahead: the emulated wireless
        // clock is reset to the host's when it joins, which isn't a real lead)
        const double ahead = EmuAnchorUs ? EmuAheadMs() : 0;
        if (!EmuAnchorUs || timestamp < EmuAnchorUs || ahead < 0 || ahead > 50)
        {
            EmuAnchorUs = timestamp;
            RealAnchorMs = EmuNowAt;
        }
    }
    if (!data || !Mine(inst)) return;
    std::lock_guard l(Lock);
    std::vector<DsBridge::Msg> msgs;
    std::vector<DsBridge::Frame> local;
    Proxy.FromLocal(mpType, aid, data, len, timestamp, msgs, local);
    if (mpType == 1 && len >= 12 + 24) { CurCmdSeq = (u16)(data[12 + 22] | data[12 + 23] << 8); UsedReplyRef = 0xFFFF; }
    for (auto& f : local) Queue[f.MPType == 2 ? 1 : 0].push_back(std::move(f));
    for (const auto& m : msgs)
        if (m.Type == DsBridge::MsgType::Leave)
            if (FILE* f = std::fopen("WideMelon bridge log.txt", "a"))
            {
                // (for the log: this side's game leaving or ending the link)
                std::fprintf(f, "[%.1f s] this game left (or ended the link); still queued here: %zu replies, %zu other frames\n",
                             NowMs() / 1000.0, Queue[1].size(), Queue[0].size());
                std::fclose(f);
                DumpCapture("this game left");
            }
    for (auto& m : msgs)
    {
        if (m.Type == DsBridge::MsgType::Reply) m.RefSeq = LastCmdSeq;
        // our ack to a real DS also says which of its replies we took this round (the CMD
        // it answers; 0xFFFF none): the packets that one carried are the ones delivered.
        // "Got it" alone it took for its latest reply, which over WiFi often comes a round
        // late; when that one was lost, its packets were lost for good
        if (m.Type == DsBridge::MsgType::Ack && RemoteIsDs && m.Body.size() == 4)
        {
            m.Body.push_back((u8)UsedReplyRef);
            m.Body.push_back((u8)(UsedReplyRef >> 8));
        }
        SendMsg(m);
        // to a real DS, each reply twice: we ack them ourselves (SendReply), so one
        // lost on the way would never be sent again (the DS drops the copy)
        if (m.Type == DsBridge::MsgType::Reply && RemoteIsDs && Cfg.LocalAck) SendMsg(m);
        // and each CMD twice: our host takes the DS's reply as proof it got this CMD's
        // packets, and over WiFi that reply is often a late one, to the CMD before; when
        // this CMD was lost on the way, its packets were lost for good (the DS stood
        // "Communicating"). The DS answers the copy with the same reply, nothing more.
        if (m.Type == DsBridge::MsgType::Cmd && RemoteIsDs) SendMsg(m);
    }
    if (!local.empty()) Arrived.notify_all();
}

bool BridgeMP::Take(int fifo, DsBridge::Frame& out, double timeoutMs)
{
    std::unique_lock l(Lock);
    const double until = NowMs() + timeoutMs;
    for (;;)
    {
        PumpDelayed();
        auto& q = Queue[fifo];
        if (!q.empty())
        {
            out = std::move(q.front());
            q.pop_front();
            return true;
        }
        const double now = NowMs();
        if (now >= until) return false;
        double wake = until;
        if (!Incoming.empty()) wake = std::min(wake, Incoming.front().Due);
        Arrived.wait_for(l, std::chrono::duration<double, std::milli>(std::max(0.05, wake - now)));
    }
}

int BridgeMP::SendPacket(int inst, u8* data, int len, u64 timestamp) { Send(inst, 0, 0, data, len, timestamp); return len; }
int BridgeMP::SendCmd(int inst, u8* data, int len, u64 timestamp) { Send(inst, 1, 0, data, len, timestamp); return len; }
int BridgeMP::SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid)
{
    Send(inst, 2, aid, data, len, timestamp);
    if (!data || !len) return len;
    {
        // for the log: a "not ready" reply (frame control 0x0158: the game's wireless code
        // had no reply armed); a long run of them is the client firmware stuck
        const bool notReady = len >= 14 && data[12] == 0x58 && data[13] == 0x01;
        std::lock_guard l(Lock);
        Window.Replies++;
        if (notReady) { Window.NotReady++; if (++NotReadyRun > Window.NotReadyRun) Window.NotReadyRun = NotReadyRun; }
        else NotReadyRun = 0;
    }
    {
        // With a real DS as host: the ack for this reply is made here, at once. The
        // game's MP layer wants it right after its reply, in its own time; the DS's
        // real one comes a network round trip later, and waiting for it took
        // WideMelon down to about 12 FPS once linked (and without the wait every
        // round timed out at the ack). The DS keeps every reply we send, late ones
        // included (they answer its next round), so "received" is true.
        std::lock_guard l(Lock);
        if (RemoteIsDs && Cfg.LocalAck && HaveAckTemplate)
        {
            DsBridge::Msg a = AckTemplate;
            if (a.Body.size() >= 4)
            {
                ++LocalAckCounter;
                a.Body[0] = (u8)LocalAckCounter; a.Body[1] = (u8)(LocalAckCounter >> 8);
                a.Body[2] = 0; a.Body[3] = 0;       // no one missed
            }
            // the sequence number of the CMD it closes, as a real host's ack has (the
            // pattern's own, the same for every ack, was a frozen one)
            a.Seq = LastCmdSeq;
            std::vector<DsBridge::Frame> frames;
            Proxy.FromRemote(a, frames);
            for (auto& f : frames)
            {
                f.Timestamp = timestamp;
                // its runahead (melonDS header, first 4 bytes): how long the local DS runs
                // before it looks for the next frame. A CMD fetched early, even one due
                // later, held the game's wireless code from getting its next reply ready
                // (it answered "not ready", and once for good). So: the CMD gap.
                const u32 ahead = (u32)Cfg.CmdGapUs;
                if (f.Data.size() >= 4)
                    for (int i = 0; i < 4; i++) f.Data[i] = (u8)(ahead >> (8 * i));
                // first in the queue: a CMD already waiting is a later one, and one handed
                // over before this ack (the ack of the reply just sent) left the game's
                // wireless code waiting for that ack for good, answering "not ready" since
                Queue[0].push_front(std::move(f));
            }
            Arrived.notify_all();
            return len;
        }
    }
    AwaitingAck = true;    // the host's ack for it: waited for, a little (RecvHostPacket)
    return len;
}
int BridgeMP::SendAck(int inst, u8* data, int len, u64 timestamp) { Send(inst, 3, 0, data, len, timestamp); return len; }

int BridgeMP::RecvPacket(int inst, u8* data, u64* timestamp)
{
    if (!Mine(inst)) return 0;
    DsBridge::Frame f;
    if (!Take(0, f, 0)) return 0;
    return Hand(f, data, timestamp);
}

int BridgeMP::Hand(const DsBridge::Frame& f, u8* data, u64* timestamp)
{
    // the client's next reply answers this CMD (it may come either way in)
    if (f.MPType == 1 && f.Data.size() >= 12 + 24)
    {
        std::lock_guard l(Lock);
        LastCmdSeq = (u16)(f.Data[12 + 22] | f.Data[12 + 23] << 8);
    }
    // A CMD never comes right on the heels of the ack before it, as on the air. A real
    // DS's CMDs carry no time we can use (they get our own clock), so one queued behind
    // an ack (always so with our local acks) was handed over a few microseconds after
    // it, in emulated time; the game's wireless code then took the two as one, lost
    // the ack, and from then on answered every CMD "not ready" (the talk or trade
    // stood waiting for good). Spaced like this, its interrupt for the ack is done first.
    u64 ts = f.Timestamp;
    if (f.MPType == 3) LastAckUs = ts;
    else if (f.MPType == 1 && LastAckUs && ts < LastAckUs + Cfg.CmdGapUs && LastAckUs + Cfg.CmdGapUs - ts < 50000)
        ts = LastAckUs + Cfg.CmdGapUs;
    std::memcpy(data, f.Data.data(), f.Data.size());
    if (timestamp) *timestamp = ts;
    return (int)f.Data.size();
}

int BridgeMP::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    // a client looking for the host's next CMD: by default it doesn't wait, it
    // takes only what has already come
    if (!Mine(inst)) return 0;
    DsBridge::Frame f;
    for (;;)
    {
        // Not waiting in general (melonDS asks on every tick while it's ahead: 20 ms each
        // time took WideMelon down to 6 FPS), except for the ack of the reply we just
        // sent: the DS's MP layer wants it right after its reply, in its own (emulated)
        // time, and with a network between, it only comes a round trip later. Without
        // this short wait nearly every round "timed out" at the ack and the game soon
        // gave up the link. Once a round, at most AckWaitMs.
        double wait = AwaitingAck ? Cfg.AckWaitMs : Cfg.ClientWaitMs;
        // With a real DS as host: WideMelon emulates a frame in a few ms, then sleeps.
        // CMDs that came while it slept were all taken at once when it woke, a moment
        // apart in emulated time, and the game's wireless code had no time to get its
        // next reply ready between them: it answered "not ready", and the game's
        // exchange stalled for good. So while this side's emulated clock is ahead of
        // real time (time the frame limiter would sleep anyway), it waits for the next
        // CMD instead, and each is taken about when it came, as on the air.
        // (at most PaceBudgetMs of such waiting per 1/60 s of real time, whatever happens)
        const double now = NowMs();
        if (now - PaceWindowStart >= 1000.0 / 60) { PaceWindowStart = now; PaceSpent = 0; }
        if (RemoteIsDs && Cfg.PaceToDs && EmuNowAt > 0)
            wait = std::max(wait, std::min({EmuAheadMs(), Cfg.PaceMaxMs, Cfg.PaceBudgetMs - PaceSpent}));
        const bool got = Take(0, f, std::max(0.0, wait));
        PaceSpent += NowMs() - now;
        // an ack we stopped waiting for is missed, as on the air: if it still comes
        // later, it's dropped (handing it over that late, long after its window, left
        // the game's wireless code stuck answering "not ready" from then on)
        if (!got) { if (AwaitingAck) AckMissed = true; AwaitingAck = false; return 0; }
        if (f.MPType == 3 && AckMissed) { AckMissed = false; continue; }
        if (f.MPType == 1) AckMissed = false;
        if (f.MPType == 1 || f.MPType == 3) AwaitingAck = false;
        // a CMD with a newer CMD already behind it is over: on the air it would have gone
        // by. Answering it would leave this side a round behind for good (every reply
        // then misses its round at the host). Its ack goes too. But an ack for a CMD we
        // did answer is always handed over, even with the next CMD right behind it (a
        // DS host often sends the two together): without it the client's MP layer
        // times out on every round and the game soon gives up the link.
        if (f.MPType == 1)
        {
            if (NewerCmdQueued()) { SkippedCmd = true; continue; }
            SkippedCmd = false;
        }
        else if (f.MPType == 3 && SkippedCmd)
        {
            SkippedCmd = false;
            continue;
        }
        return Hand(f, data, timestamp);
    }
}

bool BridgeMP::NewerCmdQueued()
{
    std::lock_guard l(Lock);
    for (const auto& q : Queue[0])
        if (q.MPType == 1) return true;
    return false;
}

u16 BridgeMP::RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask)
{
    // the local host collecting the remote's reply to the CMD it just sent: it
    // waits for it (the link's round trip), as a slow link needs
    if (!Mine(inst)) return 0;
    const bool waits = Cfg.HostWaitMs > 0;
    const double waitFrom = NowMs();
    // (within the frame's spare time: never a frame lost to waiting; see HostFitFrame)
    double budget = Cfg.HostWaitMs;
    if (Cfg.HostFitFrame && EmuNowAt > 0)
        budget = std::max(std::min(EmuAheadMs(), Cfg.HostWaitMs), std::min(Cfg.HostMinWaitMs, Cfg.HostWaitMs));
    const double until = waitFrom + budget;
    u16 cmdSeq;
    {
        std::lock_guard l(Lock);
        cmdSeq = CurCmdSeq;
    }
    u16 got = 0;
    u32 stale = 0;
    u16 usedRef = 0xFFFF;
    for (;;)
    {
        DsBridge::Frame f;
        // (once every reply is in, any more already waiting are taken too: the newest one
        // counts. Taking the oldest kept the host a round behind for good once a reply
        // was late)
        const bool allIn = (got & aidmask) == aidmask;
        if (!Take(1, f, allIn ? 0 : waits ? std::max(0.0, until - NowMs()) : 0)) break;
        const bool late = f.RefSeq != 0xFFFF && f.RefSeq != cmdSeq;
        if (late) stale++;
        // From a real DS, only a reply to this CMD counts: our host takes any reply as
        // proof the DS got this CMD's packets, and a late one (to an earlier CMD, when
        // this CMD was lost on the way) made it count lost packets as delivered: the
        // DS then waited for them for good ("Communicating", seen in a trace). Without
        // one, the host sends its packets again; the DS's own packets in the late reply
        // are sent again too (our ack tells it which reply we took: none).
        // From another WideMelon, a late reply still counts, as the client's latest word:
        // dropping it made the game resend and fall behind until talks failed (tested at
        // 5-10 ms delay); melonDS's own local multiplayer takes it too.
        if (late && RemoteIsDs) continue;
        usedRef = f.RefSeq;
        if (f.Aid >= 1 && f.Aid <= 15 && !f.Data.empty())
        {
            std::memcpy(&data[(f.Aid - 1) * 1024], f.Data.data(), std::min<size_t>(f.Data.size(), 1024));
            got |= 1 << f.Aid;
        }
    }
    std::lock_guard l(Lock);
    // the CMD the reply we took answers: told to a real DS in the ack (see Send)
    UsedReplyRef = got ? usedRef : 0xFFFF;
    Total.Exchanges++; Window.Exchanges++;
    { const double w = NowMs() - waitFrom; Window.WaitMs += w; Window.WaitMaxMs = std::max(Window.WaitMaxMs, w); }
    Total.StaleReplies += stale; Window.StaleReplies += stale;
    if ((got & aidmask) == aidmask) { Total.Answered++; Window.Answered++; }
    return got;
}

}
