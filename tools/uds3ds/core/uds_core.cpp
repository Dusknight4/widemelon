// uds_core: the 3DS local wireless service, portable (see uds_core.h). Follows Azahar's nwm_uds.cpp,
// uds_beacon.cpp, uds_connection.cpp and uds_data.cpp (release 2126.2) function by function, quirks
// included, since the other end of the link is that code.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "uds_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "uds_crypto.h"

namespace UdsCore
{

namespace
{
// NetworkStatus
constexpr u32 NotConnected = 3, ConnectedAsHost = 6, Connecting = 7, ConnectedAsClient = 9, ConnectedAsSpectator = 10;
// NetworkStatusChangeReason
constexpr u32 ConnectionEstablished = 1;
constexpr u16 BroadcastNodeId = 0xFFFF;
constexpr u16 HostNodeId = 1;
constexpr u16 NodeIdSpec = 0;
constexpr size_t MaxBeacons = 15;
constexpr double BeaconIntervalMs = 100 * 1.024;
constexpr u8 DefaultChannel = 11;
constexpr u16 ExtraCapabilities = 0x0431;
constexpr u32 EncryptedDataCutoff = 0xFA;
constexpr u8 NintendoOUI[3] = {0x00, 0x1F, 0x32};
constexpr u16 EtherSecureData = 0x876D, EtherEapol = 0x888E;
constexpr u16 EapolStart = 0x201, EapolLogoff = 0x202;
constexpr size_t LLC = 8, SecureHeader = 14, EapolStartSize = 0x30, EapolLogoffSize = 0x298, EapolNodeSize = 0x28;
constexpr size_t BeaconNodeSize = 0x1E, BeaconDataHeader = 0x12;

// NetworkInfo field offsets
constexpr size_t NI_HostMac = 0, NI_Channel = 6, NI_Oui = 0xC, NI_OuiType = 0xF, NI_WlanCommId = 0x10, NI_Id = 0x14,
                 NI_NetworkId = 0x18, NI_TotalNodes = 0x1C, NI_MaxNodes = 0x1D, NI_AppDataSize = 0x3F, NI_AppData = 0x40;
constexpr size_t AppDataMax = 0xC8;

u16 Be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
u32 Be32(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
u64 Be64(const u8* p) { return (u64)Be32(p) << 32 | Be32(p + 4); }
u16 Le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
u32 Le32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
u64 Le64(const u8* p) { return Le32(p) | (u64)Le32(p + 4) << 32; }
void PutBe16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
void PutBe32(u8* p, u32 v) { PutBe16(p, (u16)(v >> 16)); PutBe16(p + 2, (u16)v); }
void PutBe64(u8* p, u64 v) { PutBe32(p, (u32)(v >> 32)); PutBe32(p + 4, (u32)v); }
void PutLe16(u8* p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
void PutLe32(u8* p, u32 v) { PutLe16(p, (u16)v); PutLe16(p + 2, (u16)(v >> 16)); }
void PutLe64(u8* p, u64 v) { PutLe32(p, (u32)v); PutLe32(p + 4, (u32)(v >> 32)); }

std::vector<u8> LlcHeader(u16 protocol)
{
    std::vector<u8> b = {0xAA, 0xAA, 0x03, 0, 0, 0, 0, 0};
    PutBe16(&b[6], protocol);
    return b;
}

u16 FrameEtherType(const std::vector<u8>& f) { return f.size() >= LLC ? Be16(&f[6]) : 0; }
}

Service::Service(Host& h, const Mac& m) : host(h), mac(m)
{
    status.status = NotConnected;
}

Service::NodeInfo Service::ReadNodeInfo(const u8* p)
{
    NodeInfo n;
    n.friendCodeSeed = Le64(p);
    for (int i = 0; i < 10; i++) n.username[i] = Le16(p + 8 + 2 * i);
    n.networkNodeId = Le16(p + 32);
    return n;
}

void Service::WriteNodeInfo(const NodeInfo& n, u8* p)
{
    std::memset(p, 0, kNodeInfoSize);
    PutLe64(p, n.friendCodeSeed);
    for (int i = 0; i < 10; i++) PutLe16(p + 8 + 2 * i, n.username[i]);
    PutLe16(p + 32, n.networkNodeId);
}

Service::NodeInfo Service::ReadEapolNode(const u8* p)
{
    NodeInfo n;
    n.friendCodeSeed = Be64(p);
    for (int i = 0; i < 10; i++) n.username[i] = Be16(p + 8 + 2 * i);
    n.networkNodeId = Be16(p + 32);
    return n;
}

void Service::WriteEapolNode(const NodeInfo& n, u8* p)
{
    std::memset(p, 0, EapolNodeSize);
    PutBe64(p, n.friendCodeSeed);
    for (int i = 0; i < 10; i++) PutBe16(p + 8 + 2 * i, n.username[i]);
    PutBe16(p + 32, n.networkNodeId);
}

void Service::Send(Frame frame)
{
    frame.src = mac;
    host.Send(frame);
}

bool Service::Initialized()
{
    std::lock_guard<std::recursive_mutex> l(lock);
    return initialized;
}

u32 Service::Initialize(const u8 node[kNodeInfoSize], u16)
{
    std::lock_guard<std::recursive_mutex> l(lock);
    currentNode = ReadNodeInfo(node);
    initialized = true;
    status = ConnectionStatus {};
    status.status = NotConnected;
    nodeInfo.clear();
    nodeInfo.push_back(currentNode);
    channels.clear();
    return Result::Success;
}

void Service::Shutdown()
{
    std::lock_guard<std::recursive_mutex> l(lock);
    initialized = false;
    hosting = false;
    for (auto& [ch, bind] : channels) host.SignalBindEvent(bind.bindNodeId);
    channels.clear();
    nodeMap.clear();
}

u32 Service::BeginHostingNetwork(const u8* info, size_t size)
{
    {
        std::lock_guard<std::recursive_mutex> l(lock);
        networkInfo.fill(0);
        std::memcpy(networkInfo.data(), info, std::min(size, networkInfo.size()));
        if (networkInfo[NI_MaxNodes] <= 1) return Result::NotAuthorizedWrongArg;  // (the real module throws a fatal error)
        if (networkInfo[NI_AppDataSize] > AppDataMax) networkInfo[NI_AppDataSize] = AppDataMax;

        status.status = ConnectedAsHost;
        status.changeReason = ConnectionEstablished;
        std::memcpy(&networkInfo[NI_Oui], NintendoOUI, 3);
        networkInfo[NI_OuiType] = 21;   // NintendoTagId::NetworkInfo
        status.maxNodes = networkInfo[NI_MaxNodes];
        nodeInfo.assign(networkInfo[NI_MaxNodes], NodeInfo {});
        status.totalNodes = 1;
        networkInfo[NI_TotalNodes] = 1;
        status.networkNodeId = 1;
        currentNode.networkNodeId = 1;
        status.nodes[0] = status.networkNodeId;
        status.nodeBitmask |= 1;
        status.changedNodes |= 1;
        std::memcpy(&networkInfo[NI_HostMac], mac.data(), 6);
        nodeInfo[0] = currentNode;
        if (networkInfo[NI_Channel] != 0) networkChannel = networkInfo[NI_Channel];
        else networkInfo[NI_Channel] = DefaultChannel;
        hosting = true;
        beaconDue = 0.0;     // (the first beacon at the next Tick + an interval)
        nextBeaconMs = 0;
    }
    host.SignalConnectionEvent();
    return Result::Success;
}

u32 Service::DestroyNetwork()
{
    std::lock_guard<std::recursive_mutex> l(lock);
    hosting = false;
    if (status.status != ConnectedAsHost) return Result::WrongStatus;
    const u16 id = status.networkNodeId;
    status = ConnectionStatus {};
    status.status = NotConnected;
    status.networkNodeId = id;
    nodeMap.clear();
    host.SignalConnectionEvent();
    for (auto& [ch, bind] : channels) host.SignalBindEvent(bind.bindNodeId);
    channels.clear();
    return Result::Success;
}

u32 Service::EjectClient(u16 nodeId)
{
    if (nodeId == 1) return Result::NotAuthorizedWrongArg;
    std::lock_guard<std::recursive_mutex> l(lock);
    if (status.status != ConnectedAsHost) return Result::NotAuthorizedUsageState;
    Mac dest = kBroadcastMac;
    if (nodeId != BroadcastNodeId)
    {
        bool found;
        dest = NodeMac(nodeId, 0, found);
        if (!found) return Result::Success;
    }
    Frame deauth;
    deauth.type = FrameType::Deauthentication;
    deauth.channel = networkChannel;
    deauth.dst = dest;
    Send(deauth);
    if (nodeId == BroadcastNodeId)
    {
        Send(deauth);
        Send(deauth);
    }
    return Result::Success;
}

u32 Service::UpdateNetworkAttribute(u16, u8) { return Result::Success; }

void Service::ConnectToNetwork(const u8* info, size_t size, u8 type)
{
    Frame auth;
    {
        std::lock_guard<std::recursive_mutex> l(lock);
        networkInfo.fill(0);
        std::memcpy(networkInfo.data(), info, std::min(size, networkInfo.size()));
        connectionType = type;
        status.status = Connecting;
        auth.type = FrameType::Authentication;
        auth.channel = networkChannel;
        auth.data = {0, 0, 1, 0, 0, 0};    // open system, SEQ1, successful
        std::memcpy(auth.dst.data(), &networkInfo[NI_HostMac], 6);
    }
    Send(auth);
}

bool Service::ConnectedAsClientOrSpectator()
{
    std::lock_guard<std::recursive_mutex> l(lock);
    return status.status == ConnectedAsClient || status.status == ConnectedAsSpectator;
}

u32 Service::DisconnectNetwork()
{
    Frame deauth;
    {
        std::lock_guard<std::recursive_mutex> l(lock);
        const u16 id = status.networkNodeId;
        if (status.status == ConnectedAsHost)
        {
            status = ConnectionStatus {};
            status.status = ConnectedAsHost;
            status.networkNodeId = id;
            nodeMap.clear();
            return Result::WrongStatus;
        }
        status = ConnectionStatus {};
        status.status = NotConnected;
        status.networkNodeId = id;
        nodeMap.clear();
        host.SignalConnectionEvent();
        deauth.type = FrameType::Deauthentication;
        deauth.channel = networkChannel;
        std::memcpy(deauth.dst.data(), &networkInfo[NI_HostMac], 6);
    }
    Send(deauth);
    std::lock_guard<std::recursive_mutex> l(lock);
    for (auto& [ch, bind] : channels) host.SignalBindEvent(bind.bindNodeId);
    channels.clear();
    return Result::Success;
}

void Service::GetConnectionStatus(u8 out[kConnectionStatusSize])
{
    std::lock_guard<std::recursive_mutex> l(lock);
    std::memset(out, 0, kConnectionStatusSize);
    PutLe32(out, status.status);
    PutLe32(out + 4, status.changeReason);
    PutLe16(out + 8, status.networkNodeId);
    PutLe16(out + 10, status.changedNodes);
    for (u32 i = 0; i < kMaxNodes; i++) PutLe16(out + 12 + 2 * i, status.nodes[i]);
    out[44] = status.totalNodes;
    out[45] = status.maxNodes;
    PutLe16(out + 46, status.nodeBitmask);
    status.changedNodes = 0;    // (reset after each read, as Azahar does)
}

u32 Service::GetNodeInformation(u16 nodeId, u8 out[kNodeInfoSize])
{
    std::lock_guard<std::recursive_mutex> l(lock);
    if (!initialized) return Result::NotInitialized;
    for (const NodeInfo& n : nodeInfo)
        if (n.networkNodeId == nodeId)
        {
            WriteNodeInfo(n, out);
            return Result::Success;
        }
    return Result::NotFound;
}

u32 Service::RecvBeaconBroadcastData(const Mac& sender, u8* out, u32 outSize)
{
    std::list<Frame> list;
    {
        std::lock_guard<std::recursive_mutex> l(lock);
        if (sender != kBroadcastMac)
        {
            auto it = std::find_if(beacons.begin(), beacons.end(), [&](const Frame& f) { return f.src == sender; });
            if (it != beacons.end())
            {
                list.push_back(*it);
                beacons.erase(it);
            }
        }
        else
            list.swap(beacons);
    }
    std::memset(out, 0, std::min<u32>(outSize, 12));
    u32 at = 12, entries = 0;
    for (const Frame& b : list)
    {
        const u32 entrySize = 0x1C + (u32)b.data.size();
        if (at + entrySize > outSize) break;
        u8* e = out + at;
        std::memset(e, 0, 0x1C);
        PutLe32(e, entrySize);
        e[5] = b.channel;
        std::memcpy(e + 8, b.src.data(), 6);
        PutLe32(e + 0x14, entrySize);
        PutLe32(e + 0x18, 0x1C);
        std::memcpy(e + 0x1C, b.data.data(), b.data.size());
        at += entrySize;
        entries++;
    }
    if (outSize >= 12)
    {
        PutLe32(out, outSize);
        PutLe32(out + 4, at);
        PutLe32(out + 8, entries);
    }
    return Result::Success;
}

u32 Service::SetApplicationData(const u8* data, u32 size)
{
    if (size > AppDataMax) return Result::TooLarge;
    std::lock_guard<std::recursive_mutex> l(lock);
    networkInfo[NI_AppDataSize] = (u8)size;
    std::memcpy(&networkInfo[NI_AppData], data, size);
    return Result::Success;
}

u32 Service::GetApplicationData(u32 inputSize, std::vector<u8>& out)
{
    std::lock_guard<std::recursive_mutex> l(lock);
    const u8 n = networkInfo[NI_AppDataSize];
    out.clear();
    if (inputSize >= n) out.assign(&networkInfo[NI_AppData], &networkInfo[NI_AppData] + n);
    return Result::Success;
}

u32 Service::Bind(u32 bindNodeId, u32 recvBufferSize, u8 channel, u16 nodeId)
{
    if (channel == 0 || bindNodeId == 0) return Result::NotAuthorizedWrongArg;
    std::lock_guard<std::recursive_mutex> l(lock);
    if (channels.size() >= 16) return Result::OutOfMemory;
    if (recvBufferSize < 0x5F4) return Result::TooLarge;
    BindNode& b = channels[channel];    // (one bind node per channel, as Azahar)
    b.bindNodeId = bindNodeId;
    b.channel = channel;
    b.networkNodeId = nodeId;
    b.packets.clear();
    return Result::Success;
}

void Service::Unbind(u32 bindNodeId)
{
    std::lock_guard<std::recursive_mutex> l(lock);
    for (auto it = channels.begin(); it != channels.end(); ++it)
        if (it->second.bindNodeId == bindNodeId)
        {
            host.SignalBindEvent(bindNodeId);
            channels.erase(it);
            return;
        }
}

u32 Service::PullPacket(u32 bindNodeId, u32 maxOutSize, u32 maxOutSizeAligned, std::vector<u8>& out, u32& size, u16& src)
{
    const u32 buffSize = std::min<u32>(maxOutSizeAligned, 0x172) << 2;
    std::lock_guard<std::recursive_mutex> l(lock);
    size = 0;
    src = 0;
    if (status.status != ConnectedAsHost && status.status != ConnectedAsClient && status.status != ConnectedAsSpectator)
        return Result::NotAuthorizedState;
    auto it = std::find_if(channels.begin(), channels.end(), [&](const auto& c) { return c.second.bindNodeId == bindNodeId; });
    if (it == channels.end()) return Result::NotAuthorizedState;
    out.assign(buffSize, 0);
    if (it->second.packets.empty()) return Result::Success;
    const std::vector<u8>& p = it->second.packets.front();
    const u32 dataSize = Be16(&p[LLC]) - (u32)SecureHeader;
    src = Be16(&p[LLC + 12]);
    if (dataSize > maxOutSize) return Result::TooLarge;
    std::memcpy(out.data(), p.data() + LLC + SecureHeader, std::min<u32>(dataSize, buffSize));
    size = dataSize;
    it->second.packets.pop_front();
    return Result::Success;
}

u32 Service::SendTo(u16 dest, u8 channel, const u8* data, u32 size, u8 flags)
{
    std::lock_guard<std::recursive_mutex> l(lock);
    if (status.status != ConnectedAsClient && status.status != ConnectedAsHost) return Result::NotAuthorizedState;
    if (dest == 0 || dest == status.networkNodeId) return Result::NotFound;
    bool found;
    const Mac destMac = NodeMac(dest, flags, found);
    if (!found) return Result::NotFound;
    if (size > 0x5C6) return Result::TooLarge;

    Frame f;
    f.type = FrameType::Data;
    f.channel = networkChannel;
    f.dst = destMac;
    f.data = LlcHeader(EtherSecureData);
    u8 h[SecureHeader] = {};
    PutBe16(h, (u16)(size + SecureHeader));
    PutBe16(h + 4, (u16)(size + SecureHeader - 4));
    h[6] = 0;           // is_management
    h[7] = channel;
    PutBe16(h + 8, 0);  // sequence number (Azahar: always 0)
    PutBe16(h + 10, dest);
    PutBe16(h + 12, status.networkNodeId);
    f.data.insert(f.data.end(), h, h + SecureHeader);
    f.data.insert(f.data.end(), data, data + size);
    Send(f);
    return Result::Success;
}

u8 Service::GetChannel()
{
    std::lock_guard<std::recursive_mutex> l(lock);
    return status.status != NotConnected ? networkChannel : 0;
}

u32 Service::DecryptBeaconData(const u8 info[kNetworkInfoSize], const u8* tag0, size_t size0, const u8* tag1, size_t size1,
                               u8 out[kNodeInfoSize * kMaxNodes])
{
    std::memset(out, 0, kNodeInfoSize * kMaxNodes);
    if (size0 < 4) return Result::Success;
    std::vector<u8> data(tag0 + 4, tag0 + size0);
    if (size1 >= 4) data.insert(data.end(), tag1 + 4, tag1 + size1);
    u8 ctr[16] = {};
    std::memcpy(ctr, info + NI_HostMac, 6);
    PutLe32(ctr + 6, Be32(info + NI_WlanCommId));
    ctr[10] = info[NI_Id];
    PutLe32(ctr + 12, Be32(info + NI_NetworkId));
    static const u8 key[16] = {};   // (Azahar's stand-in for the NWM beacon key)
    UdsCrypto::AesCtr(key, ctr, data.data(), data.size());
    const u32 n = std::min<u32>(info[NI_MaxNodes], kMaxNodes);
    for (u32 i = 0; i < n; i++)
    {
        const size_t at = BeaconDataHeader + i * BeaconNodeSize;
        if (at + BeaconNodeSize > data.size()) break;
        NodeInfo node;
        node.friendCodeSeed = Be64(&data[at]);
        for (int j = 0; j < 10; j++) node.username[j] = Be16(&data[at + 8 + 2 * j]);
        node.networkNodeId = Be16(&data[at + 28]);
        WriteNodeInfo(node, out + i * kNodeInfoSize);
    }
    return Result::Success;
}

Mac Service::NodeMac(u16 dest, u8 flags, bool& found)
{
    found = true;
    if ((flags & 2) || dest == BroadcastNodeId) return kBroadcastMac;
    if (dest == HostNodeId)
    {
        Mac m;
        std::memcpy(m.data(), &networkInfo[NI_HostMac], 6);
        return m;
    }
    for (const auto& [m, node] : nodeMap)
        if (node.nodeId == dest && node.connected) return m;
    found = false;
    return {};
}

u16 Service::NextNodeId()
{
    for (u16 i = 0; i < status.maxNodes; ++i)
        if ((status.nodeBitmask & (1 << i)) == 0) return (u16)(i + 1);
    return 0;
}

// Azahar's own frame (not a 3DS's): every connected client's MAC and node id; the count is a 64-bit size_t,
// as Azahar on a 64-bit PC writes it
void Service::BroadcastNodeMap()
{
    Frame f;
    f.type = FrameType::NodeMap;
    f.channel = networkChannel;
    f.dst = kBroadcastMac;
    u64 count = 0;
    for (const auto& [m, node] : nodeMap)
        if (node.connected && !node.spec) count++;
    f.data.resize(8 + 8 * count);
    PutLe64(f.data.data(), count);
    size_t at = 8;
    for (const auto& [m, node] : nodeMap)
        if (node.connected && !node.spec)
        {
            std::memcpy(&f.data[at], m.data(), 6);
            PutLe16(&f.data[at + 6], node.nodeId);
            at += 8;
        }
    Send(f);
}

std::vector<u8> Service::EncryptedNodeData()
{
    std::vector<u8> buf(BeaconDataHeader, 0);   // md5 (filled below), bitmask 0
    for (const NodeInfo& n : nodeInfo)
    {
        u8 e[BeaconNodeSize] = {};
        PutBe64(e, n.friendCodeSeed);
        for (int i = 0; i < 10; i++) PutBe16(e + 8 + 2 * i, n.username[i]);
        PutBe16(e + 28, n.networkNodeId);
        buf.insert(buf.end(), e, e + BeaconNodeSize);
    }
    UdsCrypto::Md5(buf.data() + 16, buf.size() - 16, buf.data());
    u8 ctr[16] = {};
    std::memcpy(ctr, &networkInfo[NI_HostMac], 6);
    PutLe32(ctr + 6, Be32(&networkInfo[NI_WlanCommId]));
    ctr[10] = networkInfo[NI_Id];
    PutLe32(ctr + 12, Be32(&networkInfo[NI_NetworkId]));
    static const u8 key[16] = {};
    UdsCrypto::AesCtr(key, ctr, buf.data(), buf.size());
    return buf;
}

std::vector<u8> Service::BeaconFrame()
{
    std::vector<u8> b(12, 0);
    PutLe64(&b[0], 900000000);          // timestamp (15 minutes, as Azahar)
    PutLe16(&b[8], 100);                // beacon interval
    PutLe16(&b[10], ExtraCapabilities);
    const u8 ssid[10] = {0, 8};         // SSID tag: 8 zero bytes
    b.insert(b.end(), ssid, ssid + 10);
    const u8 dummy[9] = {221, 7, 0x00, 0x1F, 0x32, 20, 0x0A, 0x00, 0x00};
    b.insert(b.end(), dummy, dummy + 9);

    // network info tag (+ the application data), with the SHA-1 of everything after its header
    const u8 appSize = networkInfo[NI_AppDataSize];
    std::vector<u8> tag(54 + appSize, 0);
    tag[0] = 221;
    tag[1] = (u8)(54 - 2 + appSize);
    std::memcpy(&tag[2], &networkInfo[NI_Oui], 0x1F);
    tag[53] = appSize;
    std::memcpy(&tag[54], &networkInfo[NI_AppData], appSize);
    UdsCrypto::Sha1(&tag[2], tag.size() - 2, &tag[2 + 0x1F]);
    b.insert(b.end(), tag.begin(), tag.end());

    // the encrypted node list, in one or two tags
    const std::vector<u8> enc = EncryptedNodeData();
    const size_t first = std::min<size_t>(EncryptedDataCutoff, enc.size());
    const u8 t0[6] = {221, (u8)(6 - 2 + first), 0x00, 0x1F, 0x32, 24};
    b.insert(b.end(), t0, t0 + 6);
    b.insert(b.end(), enc.begin(), enc.begin() + first);
    if (enc.size() > EncryptedDataCutoff)
    {
        const size_t second = enc.size() - EncryptedDataCutoff;
        const u8 t1[6] = {221, (u8)(6 - 2 + second), 0x00, 0x1F, 0x32, 25};
        b.insert(b.end(), t1, t1 + 6);
        b.insert(b.end(), enc.begin() + EncryptedDataCutoff, enc.end());
    }
    return b;
}

void Service::Tick(u64 nowMs)
{
    std::lock_guard<std::recursive_mutex> l(lock);
    if (!hosting || status.status != ConnectedAsHost) return;
    if (beaconDue == 0.0) beaconDue = (double)nowMs + BeaconIntervalMs;
    if ((double)nowMs < beaconDue) return;
    beaconDue += BeaconIntervalMs;
    if (beaconDue < (double)nowMs) beaconDue = (double)nowMs + BeaconIntervalMs;
    Frame f;
    f.type = FrameType::Beacon;
    f.data = BeaconFrame();
    f.dst = kBroadcastMac;
    f.channel = networkChannel;
    Send(f);
}

void Service::OnFrame(const Frame& f)
{
    std::lock_guard<std::recursive_mutex> l(lock);
    if (!initialized) return;
    switch (f.type)
    {
    case FrameType::Beacon: HandleBeacon(f); break;
    case FrameType::Authentication: HandleAuthentication(f); break;
    case FrameType::AssociationResponse: HandleAssociationResponse(f); break;
    case FrameType::Data: HandleData(f); break;
    case FrameType::Deauthentication: HandleDeauthentication(f); break;
    case FrameType::NodeMap: HandleNodeMap(f); break;
    }
}

void Service::HandleBeacon(const Frame& f)
{
    beacons.remove_if([&](const Frame& b) { return b.src == f.src; });
    beacons.push_back(f);
    if (beacons.size() > MaxBeacons) beacons.pop_front();
}

void Service::HandleAuthentication(const Frame& f)
{
    if (f.data.size() < 6 || Le16(&f.data[2]) != 1) return;     // only SEQ1 needs an answer
    if (status.status != ConnectedAsHost) return;
    if (nodeMap.count(f.src)) return;
    if (status.maxNodes == status.totalNodes) return;           // (no reject frame, as Azahar)
    Frame auth;
    auth.type = FrameType::Authentication;
    auth.channel = networkChannel;
    auth.data = {0, 0, 2, 0, 0, 0};     // SEQ2
    auth.dst = f.src;
    nodeMap[f.src].connected = false;
    Send(auth);

    // the association response: capabilities, status, association id 1 | 0xC000, SSID "%08X" of the network id
    Frame assoc;
    assoc.type = FrameType::AssociationResponse;
    assoc.channel = networkChannel;
    assoc.dst = f.src;
    assoc.data.resize(6);
    PutLe16(&assoc.data[0], ExtraCapabilities);
    PutLe16(&assoc.data[2], 0);
    PutLe16(&assoc.data[4], 1 | 0xC000);
    char name[16];
    std::snprintf(name, sizeof(name), "%08X", (unsigned)Be32(&networkInfo[NI_NetworkId]));
    const u8 ssidHeader[2] = {0, 8};
    assoc.data.insert(assoc.data.end(), ssidHeader, ssidHeader + 2);
    assoc.data.insert(assoc.data.end(), (const u8*)name, (const u8*)name + 8);
    Send(assoc);
}

void Service::HandleAssociationResponse(const Frame& f)
{
    if (f.data.size() < 6 || Le16(&f.data[2]) != 0) return;     // (Azahar asserts success)
    if (status.status != Connecting) return;
    const u16 assocId = Le16(&f.data[4]) & 0x3FFF;

    // EAPoL-Start: our node info, to the host
    Frame start;
    start.type = FrameType::Data;
    start.channel = networkChannel;
    start.dst = f.src;
    start.data = LlcHeader(EtherEapol);
    u8 p[EapolStartSize] = {};
    PutBe16(p, EapolStart);
    PutBe16(p + 2, assocId);
    p[4] = connectionType;
    NodeInfo self = currentNode;
    self.networkNodeId = 0;
    WriteEapolNode(self, p + 8);
    start.data.insert(start.data.end(), p, p + EapolStartSize);
    Send(start);
}

void Service::HandleDeauthentication(const Frame& f)
{
    if (status.status != ConnectedAsHost) return;
    auto it = nodeMap.find(f.src);
    if (it == nodeMap.end()) return;
    const Node node = it->second;
    nodeMap.erase(it);
    if (!node.connected) return;
    auto info = std::find_if(nodeInfo.begin(), nodeInfo.end(), [&](const NodeInfo& n) { return n.networkNodeId == node.nodeId; });
    if (info == nodeInfo.end()) return;
    if (!node.spec)
    {
        status.nodeBitmask &= (u16)~(1 << (node.nodeId - 1));
        status.changedNodes |= (u16)(1 << (node.nodeId - 1));
        status.totalNodes--;
        status.nodes[node.nodeId - 1] = 0;
        networkInfo[NI_TotalNodes]--;
    }
    info->Reset();
    host.SignalConnectionEvent();
}

void Service::HandleData(const Frame& f)
{
    switch (FrameEtherType(f.data))
    {
    case EtherEapol: HandleEapol(f); break;
    case EtherSecureData: HandleSecureData(f); break;
    }
}

void Service::HandleEapol(const Frame& f)
{
    if (f.data.size() < LLC + 2) return;
    const u16 kind = Be16(&f.data[LLC]);
    if (kind == EapolStart)
    {
        if (status.status != ConnectedAsHost || f.data.size() < LLC + EapolStartSize) return;
        auto it = nodeMap.find(f.src);
        if (it == nodeMap.end() || it->second.connected) return;
        if (status.maxNodes == status.totalNodes) return;
        const u8* p = &f.data[LLC];
        const u8 type = p[4];
        NodeInfo node = ReadEapolNode(p + 8);
        if (type == 1)  // client
        {
            const u16 id = NextNodeId();
            if (id == 0) return;
            node.networkNodeId = id;
            status.nodeBitmask |= (u16)(1 << (id - 1));
            status.changedNodes |= (u16)(1 << (id - 1));
            status.nodes[id - 1] = id;
            status.totalNodes++;
            nodeInfo[id - 1] = node;
            networkInfo[NI_TotalNodes]++;
            nodeMap[f.src] = {true, false, id};
            BroadcastNodeMap();
        }
        else if (type == 2)  // spectator
            nodeMap[f.src] = {true, true, NodeIdSpec};
        else
            return;

        // EAPoL-Logoff: the node id given and every node, to everyone (as Azahar: a 3DS sends it to the new
        // client only)
        Frame logoff;
        logoff.type = FrameType::Data;
        logoff.channel = networkChannel;
        logoff.dst = kBroadcastMac;
        logoff.data = LlcHeader(EtherEapol);
        std::vector<u8> q(EapolLogoffSize, 0);
        PutBe16(&q[0], EapolLogoff);
        PutBe16(&q[4], node.networkNodeId);
        q[18] = networkInfo[NI_TotalNodes];
        q[19] = networkInfo[NI_MaxNodes];
        for (size_t i = 0; i < networkInfo[NI_MaxNodes] && i < nodeInfo.size() && i < kMaxNodes; i++)
            WriteEapolNode(nodeInfo[i], &q[24 + i * EapolNodeSize]);
        logoff.data.insert(logoff.data.end(), q.begin(), q.end());
        Send(logoff);
        host.SignalConnectionEvent();
    }
    else if (f.data.size() >= LLC + EapolLogoffSize && (status.status == Connecting || status.status == ConnectedAsClient ||
                                                        status.status == ConnectedAsSpectator))
    {
        const u8* q = &f.data[LLC];
        const u8 connected = q[18], maxNodes = q[19];
        if (status.status == Connecting)
        {
            std::memcpy(&networkInfo[NI_HostMac], f.src.data(), 6);
            networkInfo[NI_TotalNodes] = connected;
            networkInfo[NI_MaxNodes] = maxNodes;
            status.networkNodeId = Be16(&q[4]);
            status.totalNodes = connected;
            status.maxNodes = maxNodes;
            nodeInfo.assign(maxNodes, NodeInfo {});
            for (u32 i = 0; i < kMaxNodes; i++)
            {
                const NodeInfo n = ReadEapolNode(&q[24 + i * EapolNodeSize]);
                const u16 index = n.networkNodeId;
                if (!index || index > maxNodes) continue;
                status.nodeBitmask |= (u16)(1 << (index - 1));
                status.changedNodes |= (u16)(1 << (index - 1));
                status.nodes[index - 1] = index;
                nodeInfo[index - 1] = n;
            }
            status.status = connectionType == 2 ? ConnectedAsSpectator : ConnectedAsClient;
            status.changeReason = ConnectionEstablished;
            host.SignalConnectionEvent();
            host.SignalConnected();
        }
        else
        {
            // (Azahar's stand-in for the host's node broadcasts: the logoff sent to everyone)
            networkInfo[NI_TotalNodes] = connected;
            status.totalNodes = connected;
            status.nodes.fill(0);
            const u16 old = status.nodeBitmask;
            status.nodeBitmask = 0;
            nodeInfo.assign(networkInfo[NI_MaxNodes], NodeInfo {});
            for (u32 i = 0; i < kMaxNodes; i++)
            {
                const NodeInfo n = ReadEapolNode(&q[24 + i * EapolNodeSize]);
                const u16 index = n.networkNodeId;
                if (!index || index > nodeInfo.size()) continue;
                status.nodeBitmask |= (u16)(1 << (index - 1));
                status.nodes[index - 1] = index;
                nodeInfo[index - 1] = n;
            }
            status.changedNodes = old ^ status.nodeBitmask;
            host.SignalConnectionEvent();
        }
    }
}

void Service::HandleSecureData(const Frame& f)
{
    if (f.data.size() < LLC + SecureHeader) return;
    if (status.status != ConnectedAsHost && status.status != ConnectedAsClient && status.status != ConnectedAsSpectator)
        return;
    const u8* h = &f.data[LLC];
    const u16 dest = Be16(h + 10), src = Be16(h + 12);
    const u8 isManagement = h[6], channel = h[7];
    if (src == status.networkNodeId) return;    // (our own)
    if (dest != status.networkNodeId && dest != BroadcastNodeId)
    {
        // not for us: only the host routes (by broadcasting it)
        if (f.dst != kBroadcastMac && status.status != ConnectedAsHost) return;
        if (status.status == ConnectedAsHost && dest != BroadcastNodeId)
        {
            Frame out = f;
            out.dst = kBroadcastMac;
            Send(out);
        }
        return;
    }
    if (isManagement) return;
    auto it = channels.find(channel);
    if (it == channels.end()) return;
    if (it->second.networkNodeId != BroadcastNodeId && it->second.networkNodeId != src) return;
    it->second.packets.push_back(f.data);
    host.SignalBindEvent(it->second.bindNodeId);
}

void Service::HandleNodeMap(const Frame& f)
{
    if (status.status == ConnectedAsHost || f.data.size() < 8) return;
    nodeMap.clear();
    const u64 count = Le64(f.data.data());
    size_t at = 8;
    for (u64 i = 0; i < count && at + 8 <= f.data.size(); i++, at += 8)
    {
        Mac m;
        std::memcpy(m.data(), &f.data[at], 6);
        nodeMap[m].connected = true;
        nodeMap[m].nodeId = Le16(&f.data[at + 6]);
    }
}

}
