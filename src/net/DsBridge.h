/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef DSBRIDGE_H
#define DSBRIDGE_H

#include <cstdint>
#include <vector>

// WideMelon: a bridge for local wireless (Nintendo's "MP" multiplayer) between
// an emulated DS and a remote one that can't exchange raw radio frames, such as
// a real DS whose wireless library has been hooked by nds-bootstrap.
//
// On a DS, the wireless hardware sends beacons, answers the join handshake and
// runs the command -> reply -> ack round by itself; the game's wireless library
// only builds and reads the frame *bodies*. So the two ends exchange messages
// (below) carrying those bodies unchanged, and a Proxy, standing in for the
// remote DS, makes up for the local emulated DS the frames its hardware would
// have seen: beacons, the auth/association handshake, commands, replies, acks.
//
// Frames here are melonDS frames: a 12-byte header (bytes 0-7 from the sender's
// transmit buffer, of which melonDS reads bytes 0-3 of an ack as how far a
// client may run ahead; byte 8 rate, byte 9 channel, bytes 10-11 the 802.11
// length) followed by the 802.11 frame and its 4-byte FCS.
namespace DsBridge
{

enum class MsgType : uint8_t
{
    HostInfo = 1,   // a host's beacon: it can be joined
    Join,           // a client asks to join (its association request)
    JoinOk,         // the host accepts (its association response, with the client's AID)
    Cmd,            // the host's command for this round
    Reply,          // a client's reply to it
    Ack,            // the host closes the round
    Leave,          // gone (disassociated)
    Hello,          // keepalive, about once a second: lets a listening end learn where the other is
    Log,            // a real DS's log text (it has nowhere else to keep it): goes in the bridge log
};

struct Msg
{
    MsgType Type = MsgType::HostInfo;
    uint8_t Mac[6] {};      // the sender's MAC
    uint8_t Channel = 0;    // the channel it's on
    uint16_t Aid = 0;       // Reply: the client's; JoinOk: the one given
    uint16_t Seq = 0;       // 802.11 sequence control of the original frame
    uint16_t RefSeq = 0xFFFF; // Reply: Seq of the CMD it answers (0xFFFF: unknown)
    uint16_t FrameCtl = 0;  // 802.11 frame control of the original frame
    uint16_t Duration = 0;
    uint64_t Timestamp = 0; // the sender's microsecond clock when it sent it
    uint8_t EmuHeader[8] {};// melonDS header bytes 0-7 (zero from a real DS)
    std::vector<uint8_t> Body; // the frame body, without the FCS

    std::vector<uint8_t> Serialize() const;
    bool Parse(const uint8_t* data, size_t len);
};

// a frame for the local emulated DS, and how melonDS's MP layer delivers it
struct Frame
{
    uint32_t MPType = 0;    // 0 regular, 1 CMD, 2 reply, 3 ack
    uint16_t Aid = 0;       // for replies
    uint16_t RefSeq = 0xFFFF; // for replies: Seq of the CMD answered (0xFFFF: unknown)
    uint64_t Timestamp = 0;
    std::vector<uint8_t> Data;
};

// Stands in for one remote DS towards the local emulated one.
class Proxy
{
public:
    // a frame the local DS sent: messages for the remote, and any frames the
    // proxy answers the local DS with right away (auth response)
    void FromLocal(uint32_t mpType, uint16_t aid, const uint8_t* frame, int len, uint64_t timestamp,
                   std::vector<Msg>& toRemote, std::vector<Frame>& toLocal);
    // a message from the remote: frames for the local DS
    void FromRemote(const Msg& msg, std::vector<Frame>& toLocal);

    // a remote whose clock and melonDS header mean nothing here (a real DS, or an
    // emulator running at its own pace): frames for the local DS then carry the
    // local DS's own clock, so it takes them as soon as they come, and no header
    void SetForeignRemote(bool foreign) { ForeignRemote = foreign; }

private:
    Frame MakeFrame(uint32_t mpType, uint16_t frameCtl, uint16_t duration, const uint8_t* addr1, const uint8_t* addr2,
                    const uint8_t* addr3, uint16_t seq, const uint8_t* body, size_t bodyLen, uint8_t channel,
                    uint64_t timestamp, const uint8_t* emuHeader = nullptr);
    uint16_t NextSeq() { SeqCounter = (uint16_t)(SeqCounter + 0x10); return SeqCounter; }

    uint8_t LocalMac[6] {};
    bool LocalMacKnown = false;
    uint8_t LocalChannel = 0;
    uint64_t LocalClock = 0;        // the local DS's clock, as of its last frame

    uint8_t RemoteMac[6] {};
    uint8_t RemoteChannel = 0;
    bool RemoteIsHost = false;      // it beacons
    std::vector<uint8_t> PendingJoin;   // a remote client's association request, until auth is done
    uint16_t SeqCounter = 0;
    bool ForeignRemote = false;
};

}

#endif // DSBRIDGE_H
