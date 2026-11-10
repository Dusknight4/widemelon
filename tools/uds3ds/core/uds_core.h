// uds_core: the 3DS local wireless service (UDS, nwm::UDS), portable: a port of Azahar's (src/core/hle/
// service/nwm, release 2126.2) without its emulator, so the very same code runs inside a game on a real 3DS
// (the MultiMelon Wifi plugin) and on a PC (tests against Azahar). It speaks Azahar's 802.11 frames (beacons
// with its zero-key encrypted node list, open authentication, association, EAPoL start/logoff, plain secure
// data frames, and its NodeMap frame), so a 3DS running it plays with MultiMelon's 3DS core over UdsLink.
//
// The game's requests map to the methods below (Azahar's ...HLE functions). The platform supplies a Host:
// where frames go, and the events the game waits on.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <vector>

namespace UdsCore
{

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using Mac = std::array<u8, 6>;
constexpr Mac kBroadcastMac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Azahar's Network::WifiPacket::PacketType (what UdsLink carries as "type")
enum class FrameType : u8 { Beacon = 0, Data = 1, Authentication = 2, AssociationResponse = 3, Deauthentication = 4, NodeMap = 5 };

struct Frame
{
    FrameType type = FrameType::Data;
    std::vector<u8> data;   // the 802.11 frame body (Azahar's WifiPacket::data)
    Mac src {}, dst {};
    u8 channel = 0;
};

// structures the game exchanges with the service (little-endian, as on the 3DS)
constexpr size_t kNodeInfoSize = 40;            // NodeInfo
constexpr size_t kConnectionStatusSize = 0x30;  // ConnectionStatus
constexpr size_t kNetworkInfoSize = 0x108;      // NetworkInfo (udsNetworkStruct)
constexpr u32 kMaxNodes = 16;

// 3DS result codes (description | module << 10 | summary << 21 | level << 27; module UDS = 68)
namespace Result
{
constexpr u32 Make(u32 desc, u32 summary, u32 level) { return desc | (68u << 10) | (summary << 21) | (level << 27); }
constexpr u32 Success = 0;
constexpr u32 NotAuthorizedWrongArg = Make(1002, 8, 28);
constexpr u32 NotAuthorizedState = Make(1002, 5, 25);
constexpr u32 NotAuthorizedUsageState = Make(1002, 5, 28);
constexpr u32 OutOfMemory = Make(1011, 3, 25);
constexpr u32 TooLarge = Make(1001, 8, 28);
constexpr u32 NotFound = Make(1018, 8, 25);
constexpr u32 NotInitialized = Make(1016, 10, 25);
constexpr u32 WrongStatus = Make(490, 5, 25);
constexpr u32 Timeout = Make(1022, 9, 25);
}

class Host
{
public:
    virtual ~Host() = default;
    // a frame for the network (its src is this console's MAC); mustn't block
    virtual void Send(const Frame& frame) = 0;
    // the connection status event (the one Initialize hands to the game)
    virtual void SignalConnectionEvent() = 0;
    // the data event of a bind node
    virtual void SignalBindEvent(u32 bindNodeId) = 0;
    // ConnectToNetwork's connection has been made (its caller may return)
    virtual void SignalConnected() = 0;
};

class Service
{
public:
    Service(Host& host, const Mac& mac);

    bool Initialized();
    u32 Initialize(const u8 nodeInfo[kNodeInfoSize], u16 version);
    void Shutdown();
    // the network info (0x108 bytes; the deprecated command gives 0x3C)
    u32 BeginHostingNetwork(const u8* networkInfo, size_t size);
    u32 DestroyNetwork();
    u32 EjectClient(u16 networkNodeId);
    u32 UpdateNetworkAttribute(u16 bitmask, u8 flag);
    // starts joining; the request completes when SignalConnected comes (or after 5 s: Result::Timeout)
    void ConnectToNetwork(const u8* networkInfo, size_t size, u8 connectionType);
    bool ConnectedAsClientOrSpectator();
    u32 DisconnectNetwork();
    void GetConnectionStatus(u8 out[kConnectionStatusSize]);
    u32 GetNodeInformation(u16 networkNodeId, u8 out[kNodeInfoSize]);
    // RecvBeaconBroadcastData's output buffer (the game's: outSize bytes), beacons from `sender` or everyone
    u32 RecvBeaconBroadcastData(const Mac& sender, u8* out, u32 outSize);
    u32 SetApplicationData(const u8* data, u32 size);
    // the application data, if it fits in inputSize (else empty)
    u32 GetApplicationData(u32 inputSize, std::vector<u8>& out);
    u32 Bind(u32 bindNodeId, u32 recvBufferSize, u8 dataChannel, u16 networkNodeId);
    void Unbind(u32 bindNodeId);
    // out: the static buffer the reply carries (its size as Azahar makes it); size: the packet's
    u32 PullPacket(u32 bindNodeId, u32 maxOutSize, u32 maxOutSizeAligned, std::vector<u8>& out, u32& size, u16& srcNodeId);
    u32 SendTo(u16 destNodeId, u8 dataChannel, const u8* data, u32 size, u8 flags);
    u8 GetChannel();
    // out: 16 NodeInfo
    u32 DecryptBeaconData(const u8 networkInfo[kNetworkInfoSize], const u8* tag0, size_t size0, const u8* tag1,
                          size_t size1, u8 out[kNodeInfoSize * kMaxNodes]);

    // a frame from the network (from any thread)
    void OnFrame(const Frame& frame);
    // call often (every few ms): beacons go out every 102.4 ms while hosting
    void Tick(u64 nowMs);

private:
    struct NodeInfo
    {
        u64 friendCodeSeed = 0;
        std::array<u16, 10> username {};
        u16 networkNodeId = 0;
        void Reset() { *this = NodeInfo {}; }
    };
    struct ConnectionStatus
    {
        u32 status = 0, changeReason = 0;
        u16 networkNodeId = 0, changedNodes = 0;
        std::array<u16, kMaxNodes> nodes {};
        u8 totalNodes = 0, maxNodes = 0;
        u16 nodeBitmask = 0;
    };
    struct BindNode
    {
        u32 bindNodeId = 0;
        u8 channel = 0;
        u16 networkNodeId = 0;
        std::deque<std::vector<u8>> packets;
    };
    struct Node
    {
        bool connected = false, spec = false;
        u16 nodeId = 0;
    };

    void Send(Frame frame);
    Mac NodeMac(u16 destNodeId, u8 flags, bool& found);
    u16 NextNodeId();
    void BroadcastNodeMap();
    std::vector<u8> BeaconFrame();
    std::vector<u8> EncryptedNodeData();

    void HandleBeacon(const Frame& f);
    void HandleAuthentication(const Frame& f);
    void HandleAssociationResponse(const Frame& f);
    void HandleDeauthentication(const Frame& f);
    void HandleData(const Frame& f);
    void HandleEapol(const Frame& f);
    void HandleSecureData(const Frame& f);
    void HandleNodeMap(const Frame& f);

    static NodeInfo ReadNodeInfo(const u8* p);          // the game's layout (LE)
    static void WriteNodeInfo(const NodeInfo& n, u8* p);
    static NodeInfo ReadEapolNode(const u8* p);         // EAPoLNodeInfo (BE)
    static void WriteEapolNode(const NodeInfo& n, u8* p);

    Host& host;
    Mac mac;
    std::recursive_mutex lock;
    bool initialized = false;
    ConnectionStatus status;
    std::vector<NodeInfo> nodeInfo;     // the network's nodes (max_nodes of them; the host first)
    NodeInfo currentNode;
    u8 connectionType = 1;
    std::map<u8, BindNode> channels;    // by data channel
    u8 networkChannel = 11;
    std::array<u8, kNetworkInfoSize> networkInfo {};
    std::map<Mac, Node> nodeMap;
    std::list<Frame> beacons;           // the last beacons heard (one per sender, at most 15)
    bool hosting = false;
    u64 nextBeaconMs = 0;
    double beaconDue = 0.0;
};

}
