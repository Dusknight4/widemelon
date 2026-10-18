/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#include <algorithm>
#include <cstring>
#include "DsBridge.h"

namespace DsBridge
{

// multicast addresses of the MP round
static const uint8_t kMacBroadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t kMacMPCmd[6]     = { 0x03, 0x09, 0xbf, 0x00, 0x00, 0x00 };
static const uint8_t kMacMPReply[6]   = { 0x03, 0x09, 0xbf, 0x00, 0x00, 0x10 };
static const uint8_t kMacMPAck[6]     = { 0x03, 0x09, 0xbf, 0x00, 0x00, 0x03 };

static uint16_t Get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void Put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// ---- messages on the wire: "DSB1", then the fields, little-endian ----

std::vector<uint8_t> Msg::Serialize() const
{
    std::vector<uint8_t> out(4 + 1 + 6 + 1 + 2 * 5 + 8 + 8 + 2 + Body.size());
    uint8_t* p = out.data();
    std::memcpy(p, "DSB1", 4); p += 4;
    *p++ = (uint8_t)Type;
    std::memcpy(p, Mac, 6); p += 6;
    *p++ = Channel;
    Put16(p, Aid); p += 2;
    Put16(p, Seq); p += 2;
    Put16(p, RefSeq); p += 2;
    Put16(p, FrameCtl); p += 2;
    Put16(p, Duration); p += 2;
    for (int i = 0; i < 8; i++) *p++ = (uint8_t)(Timestamp >> (8 * i));
    std::memcpy(p, EmuHeader, 8); p += 8;
    Put16(p, (uint16_t)Body.size()); p += 2;
    if (!Body.empty()) std::memcpy(p, Body.data(), Body.size());
    return out;
}

bool Msg::Parse(const uint8_t* data, size_t len)
{
    const size_t head = 4 + 1 + 6 + 1 + 2 * 5 + 8 + 8 + 2;
    if (len < head || std::memcmp(data, "DSB1", 4) != 0) return false;
    const uint8_t* p = data + 4;
    Type = (MsgType)*p++;
    std::memcpy(Mac, p, 6); p += 6;
    Channel = *p++;
    Aid = Get16(p); p += 2;
    Seq = Get16(p); p += 2;
    RefSeq = Get16(p); p += 2;
    FrameCtl = Get16(p); p += 2;
    Duration = Get16(p); p += 2;
    Timestamp = 0;
    for (int i = 0; i < 8; i++) Timestamp |= (uint64_t)*p++ << (8 * i);
    std::memcpy(EmuHeader, p, 8); p += 8;
    const uint16_t bodyLen = Get16(p); p += 2;
    if (len < head + bodyLen) return false;
    Body.assign(p, p + bodyLen);
    return true;
}

// ---- frames ----

Frame Proxy::MakeFrame(uint32_t mpType, uint16_t frameCtl, uint16_t duration, const uint8_t* addr1, const uint8_t* addr2,
                       const uint8_t* addr3, uint16_t seq, const uint8_t* body, size_t bodyLen, uint8_t channel,
                       uint64_t timestamp, const uint8_t* emuHeader)
{
    Frame f;
    f.MPType = mpType;
    // a foreign clock would make the local DS hold the frame until it catches up,
    // or take it as long overdue: the local clock as of its last frame means "now"
    f.Timestamp = ForeignRemote ? (LocalClock ? LocalClock : 1) : timestamp;
    if (ForeignRemote) emuHeader = nullptr;
    const size_t len80211 = 24 + bodyLen + 4;   // with the FCS
    f.Data.assign(12 + len80211, 0);
    uint8_t* p = f.Data.data();
    if (emuHeader) std::memcpy(p, emuHeader, 8);
    p[8] = 0x14;            // 2 Mbit/s, as the games use
    p[9] = channel;
    Put16(p + 10, (uint16_t)len80211);
    p += 12;
    Put16(p, frameCtl);
    Put16(p + 2, duration);
    std::memcpy(p + 4, addr1, 6);
    std::memcpy(p + 10, addr2, 6);
    std::memcpy(p + 16, addr3, 6);
    Put16(p + 22, seq);
    if (bodyLen) std::memcpy(p + 24, body, bodyLen);
    return f;
}

void Proxy::FromLocal(uint32_t mpType, uint16_t aid, const uint8_t* frame, int len, uint64_t timestamp,
                      std::vector<Msg>& toRemote, std::vector<Frame>& toLocal)
{
    if (len < 12 + 24) return;
    const uint8_t* hdr = frame + 12;
    const uint16_t fc = Get16(hdr);
    const uint8_t* addr1 = hdr + 4;
    const uint8_t* addr2 = hdr + 10;
    const unsigned type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
    const uint8_t* body = hdr + 24;
    const int bodyLen = len - 12 - 24 - 4 > 0 ? len - 12 - 24 - 4 : 0;

    std::memcpy(LocalMac, addr2, 6);    // the local DS transmits everything it sends
    LocalMacKnown = true;
    LocalChannel = frame[9];
    if (timestamp) LocalClock = timestamp;

    Msg m;
    std::memcpy(m.Mac, LocalMac, 6);
    m.Channel = LocalChannel;
    m.Seq = Get16(hdr + 22);
    m.FrameCtl = fc;
    m.Duration = Get16(hdr + 2);
    m.Timestamp = timestamp;
    std::memcpy(m.EmuHeader, frame, 8);
    m.Body.assign(body, body + bodyLen);

    if (type == 0) {
        switch (subtype) {
            case 8:     // beacon: the local DS hosts
                m.Type = MsgType::HostInfo;
                toRemote.push_back(std::move(m));
                break;

            case 11: {  // authentication
                if (bodyLen < 6) break;
                const uint16_t authSeq = Get16(body + 2);
                if (authSeq == 1 && RemoteIsHost && std::memcmp(addr1, RemoteMac, 6) == 0) {
                    // joining the remote host: its hardware would accept right away
                    const uint8_t resp[6] = { 0, 0, 2, 0, 0, 0 };
                    toLocal.push_back(MakeFrame(0, 0x00B0, 0, LocalMac, RemoteMac, RemoteMac, NextSeq(),
                                                resp, sizeof(resp), LocalChannel, LocalClock));
                } else if (authSeq == 2 && !PendingJoin.empty() && std::memcmp(addr1, RemoteMac, 6) == 0) {
                    // the local host let the remote client authenticate: now its association request
                    toLocal.push_back(MakeFrame(0, 0x0000, 0, LocalMac, RemoteMac, LocalMac, NextSeq(),
                                                PendingJoin.data(), PendingJoin.size(), LocalChannel, LocalClock));
                    PendingJoin.clear();
                }
                break;
            }

            case 0:     // association request: the local DS joins the remote host
                m.Type = MsgType::Join;
                toRemote.push_back(std::move(m));
                break;

            case 1:     // association response: the local host takes the remote client in
                if (bodyLen >= 6) m.Aid = Get16(body + 4);
                m.Type = MsgType::JoinOk;
                toRemote.push_back(std::move(m));
                break;

            case 10:    // disassociation
            case 12:    // deauthentication
                // the reason, then who it's for (a DS remote checks it's meant for it)
                m.Type = MsgType::Leave;
                m.Body.resize(2);
                m.Body.insert(m.Body.end(), addr1, addr1 + 6);
                toRemote.push_back(std::move(m));
                break;

            default:    // probe requests and the like: not for the remote
                break;
        }
        return;
    }

    if (type != 2) return;
    if (mpType == 1 || std::memcmp(addr1, kMacMPCmd, 6) == 0) {
        m.Type = MsgType::Cmd;
    } else if (mpType == 3 || std::memcmp(addr1, kMacMPAck, 6) == 0) {
        m.Type = MsgType::Ack;
    } else if (mpType == 2) {
        m.Type = MsgType::Reply;
        m.Aid = aid;
    } else {
        return;
    }
    toRemote.push_back(std::move(m));
}

void Proxy::FromRemote(const Msg& m, std::vector<Frame>& toLocal)
{
    switch (m.Type) {
        case MsgType::HostInfo:
            // the remote hosts: its beacon, on its channel
            std::memcpy(RemoteMac, m.Mac, 6);
            RemoteChannel = m.Channel;
            RemoteIsHost = true;
            toLocal.push_back(MakeFrame(0, 0x0080, 0, kMacBroadcast, RemoteMac, RemoteMac, m.Seq,
                                        m.Body.data(), m.Body.size(), m.Channel, m.Timestamp, m.EmuHeader));
            break;

        case MsgType::Join:
            // a remote client joins the local host: authentication first
            if (!LocalMacKnown) break;
            std::memcpy(RemoteMac, m.Mac, 6);
            RemoteIsHost = false;
            PendingJoin = m.Body;
            {
                const uint8_t auth[6] = { 0, 0, 1, 0, 0, 0 };
                toLocal.push_back(MakeFrame(0, 0x00B0, 0, LocalMac, RemoteMac, LocalMac, NextSeq(),
                                            auth, sizeof(auth), LocalChannel, m.Timestamp ? m.Timestamp : LocalClock));
            }
            break;

        case MsgType::JoinOk:
            // the remote host took the local DS in
            if (!LocalMacKnown) break;
            toLocal.push_back(MakeFrame(0, 0x0010, 0, LocalMac, m.Mac, m.Mac, m.Seq,
                                        m.Body.data(), m.Body.size(), m.Channel, m.Timestamp, m.EmuHeader));
            break;

        case MsgType::Cmd:
            toLocal.push_back(MakeFrame(1, m.FrameCtl, m.Duration, kMacMPCmd, m.Mac, m.Mac, m.Seq,
                                        m.Body.data(), m.Body.size(), m.Channel, m.Timestamp, m.EmuHeader));
            break;

        case MsgType::Reply:
            if (!LocalMacKnown) break;
            {
                Frame f = MakeFrame(2, m.FrameCtl, m.Duration, LocalMac, m.Mac, kMacMPReply, m.Seq,
                                    m.Body.data(), m.Body.size(), LocalChannel, m.Timestamp, m.EmuHeader);
                f.Aid = m.Aid;
                f.RefSeq = m.RefSeq;
                toLocal.push_back(std::move(f));
            }
            break;

        case MsgType::Ack:
            // (only the air's 4 bytes: a wmhle host adds, per child, which reply it took,
            // for wmhle children; the emulated firmware gets the ack as on the air)
            toLocal.push_back(MakeFrame(3, m.FrameCtl, m.Duration, kMacMPAck, m.Mac, m.Mac, m.Seq,
                                        m.Body.data(), std::min<size_t>(m.Body.size(), 4), m.Channel, m.Timestamp, m.EmuHeader));
            break;

        case MsgType::Leave:
            if (!LocalMacKnown) break;
            {
                const uint8_t reason[2] = { 3, 0 };
                toLocal.push_back(MakeFrame(0, 0x00C0, 0, LocalMac, m.Mac, RemoteIsHost ? m.Mac : LocalMac, NextSeq(),
                                            reason, sizeof(reason), m.Channel, m.Timestamp, m.EmuHeader));
            }
            break;

        case MsgType::Hello:
            break;
    }
}

}
