// fake3ds: a "3DS" on the PC for testing uds_core against Azahar: uds_core over MultiMelon's UdsLink, driven
// as the udstest homebrew drives libctru (scan for the "udstest" network, join it or host it, then for 20 s
// send a numbered packet every 10 frames and count the ones received), so it plays either side against
// rig3ds running udstest.3dsx. It also checks what a game would: the beacon's SHA-1, and the node list
// DecryptBeaconData gives (the host's name).
//
// usage: fake3ds PORT PEERS [auto|host|join] [NAME]
//   PEERS "ip:port[,...]" (hellos go there; loopback only). auto: scan 3 times, join if found, else host.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "UdsLink.h"
#include "uds_core.h"
#include "uds_crypto.h"

using namespace UdsCore;

namespace
{
constexpr u32 kWlanCommId = 0x4D4D3344;     // "MM3D", as udstest
constexpr u32 kMagic = 0x55445354;          // "UDST"
constexpr u8 kChannel = 1;
constexpr u32 kBindId = 1;

u64 NowMs()
{
    return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

u32 Be32(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
u32 Le32(const u8* p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
u16 Le16(const u8* p) { return (u16)(p[0] | p[1] << 8); }

// (FAKE_DUMP=file: the frames sent and received, the first few of each kind, in hex: test vectors)
FILE* g_dump = nullptr;
std::mutex g_dumpLock;
void Dump(const char* dir, const Frame& f)
{
    if (!g_dump) return;
    static const char* names[] = {"Beacon", "Data", "Authentication", "AssociationResponse", "Deauthentication", "NodeMap"};
    std::lock_guard<std::mutex> l(g_dumpLock);
    // data frames: EAPoL (all) and secure data (a few); others: a few of each, per direction
    static std::map<std::string, int> seen;
    std::string kind = std::string(dir) + ((u8)f.type < 6 ? names[(u8)f.type] : "?");
    if (f.type == FrameType::Data && f.data.size() >= 8) kind += (f.data[6] == 0x88 && f.data[7] == 0x8E) ? "/EAPoL" : "/SecureData";
    if (++seen[kind] > (kind.find("EAPoL") != std::string::npos ? 4 : 2)) return;
    std::fprintf(g_dump, "%s %s channel %u  src %02X:%02X:%02X:%02X:%02X:%02X  dst %02X:%02X:%02X:%02X:%02X:%02X  %u bytes\n", dir,
                 (u8)f.type < 6 ? names[(u8)f.type] : "?", f.channel, f.src[0], f.src[1], f.src[2], f.src[3], f.src[4], f.src[5],
                 f.dst[0], f.dst[1], f.dst[2], f.dst[3], f.dst[4], f.dst[5], (unsigned)f.data.size());
    for (size_t i = 0; i < f.data.size(); i += 16)
    {
        std::fprintf(g_dump, "  %04zx:", i);
        for (size_t j = i; j < i + 16 && j < f.data.size(); j++) std::fprintf(g_dump, " %02X", f.data[j]);
        std::fprintf(g_dump, "\n");
    }
    std::fflush(g_dump);
}

struct LinkHost final : Host
{
    std::mutex m;
    std::condition_variable cv;
    bool connected = false;
    std::atomic<unsigned> connectionEvents {0}, bindEvents {0};
    Mac self {};

    void Send(const Frame& f) override
    {
        Frame out = f;
        out.src = self;
        Dump("SENT", out);
        UdsLink::Send(f.dst.data(), (u8)f.type, f.channel, f.data.data(), f.data.size());
    }
    void SignalConnectionEvent() override { connectionEvents++; }
    void SignalBindEvent(u32) override { bindEvents++; }
    void SignalConnected() override
    {
        std::lock_guard<std::mutex> l(m);
        connected = true;
        cv.notify_all();
    }
};

struct ScanResult
{
    u8 network[kNetworkInfoSize] {};
    bool shaOk = false;
    std::string hostName;
    unsigned nodes = 0;
};

std::string Utf16Name(const u8* p)
{
    std::string s;
    for (int i = 0; i < 10; i++)
    {
        const u16 c = Le16(p + 2 * i);
        if (!c) break;
        s += c < 0x80 ? (char)c : '?';
    }
    return s;
}

// a beacon entry (RecvBeaconBroadcastData's output) into a network struct, as libctru's scan does
bool ParseBeacon(Service& uds, const u8* entry, ScanResult& out)
{
    const u32 total = Le32(entry), headerSize = Le32(entry + 0x18);
    if (total < headerSize + 12) return false;
    const u8* frame = entry + headerSize;
    const size_t size = total - headerSize;
    std::memset(out.network, 0, sizeof(out.network));
    std::memcpy(out.network, entry + 8, 6);     // host MAC
    out.network[6] = entry[5];                  // channel
    out.network[8] = 1;                         // initialized
    const u8 *tag0 = nullptr, *tag1 = nullptr;
    size_t size0 = 0, size1 = 0;
    bool info = false;
    for (size_t at = 12; at + 2 <= size;)
    {
        const u8 id = frame[at], len = frame[at + 1];
        const u8* d = frame + at + 2;
        if (at + 2 + len > size) break;
        if (id == 221 && len >= 4 && d[0] == 0x00 && d[1] == 0x1F && d[2] == 0x32)
        {
            if (d[3] == 21 && len >= 0x34)
            {
                std::memcpy(out.network + 0xC, d, 0x1F);
                const u8 appSize = d[0x33];
                out.network[0x3F] = appSize;
                std::memcpy(out.network + 0x40, d + 0x34, std::min<size_t>(appSize, 0xC8));
                // SHA-1 of the tag with its hash field zeroed
                std::vector<u8> copy(d, d + len);
                std::memset(&copy[0x1F], 0, 20);
                u8 sha[20];
                UdsCrypto::Sha1(copy.data(), copy.size(), sha);
                out.shaOk = std::memcmp(sha, d + 0x1F, 20) == 0;
                info = true;
            }
            else if (d[3] == 24) { tag0 = d; size0 = len; }
            else if (d[3] == 25) { tag1 = d; size1 = len; }
        }
        at += 2 + len;
    }
    if (!info || !tag0) return false;
    static u8 nodes[kNodeInfoSize * kMaxNodes];
    uds.DecryptBeaconData(out.network, tag0, size0, tag1, size1, nodes);
    out.hostName = Utf16Name(nodes + 8);
    out.nodes = 0;
    for (u32 i = 0; i < kMaxNodes; i++) if (Le16(nodes + i * kNodeInfoSize + 32)) out.nodes++;
    return true;
}

bool Scan(Service& uds, ScanResult& found)
{
    static u8 buf[0x4000];
    SleepMs(300);   // (Azahar's RecvBeaconBroadcastData puts the caller to sleep for 300 ms)
    uds.RecvBeaconBroadcastData(kBroadcastMac, buf, sizeof(buf));
    const u32 entries = Le32(buf + 8);
    u32 at = 12;
    for (u32 i = 0; i < entries; i++)
    {
        ScanResult r;
        const u32 entrySize = Le32(buf + at);
        if (ParseBeacon(uds, buf + at, r) && Be32(r.network + 0x10) == kWlanCommId && r.network[0x14] == 0)
        {
            found = r;
            return true;
        }
        at += entrySize;
    }
    return false;
}
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: fake3ds PORT PEERS [auto|host|join] [NAME]\n");
        return 2;
    }
    const std::string mode = argc > 3 ? argv[3] : "auto";
    const std::string name = argc > 4 ? argv[4] : "Fake3DS";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("fake3ds (%s)\n", mode.c_str());
    std::printf("crypto self-test: %s\n", UdsCrypto::SelfTest() ? "pass" : "FAIL");

    std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    Mac mac = {0x40, 0xF4, 0x07, (u8)rng(), (u8)rng(), (u8)rng()};
    if (const char* m = std::getenv("FAKE_MAC"))     // (tests: a fixed MAC, as a console restarted)
    {
        unsigned v[6];
        if (std::sscanf(m, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
            for (int i = 0; i < 6; i++) mac[i] = (u8)v[i];
    }
    const u64 seconds = std::getenv("FAKE_SECONDS") ? (u64)std::atoi(std::getenv("FAKE_SECONDS")) : 20;
    const bool vanish = std::getenv("FAKE_VANISH") != nullptr;  // (tests: gone without a word, as a console switched off)
    LinkHost host;
    host.self = mac;
    if (const char* d = std::getenv("FAKE_DUMP")) g_dump = std::fopen(d, "w");
    Service uds(host, mac);

    UdsLink::Config cfg;
    cfg.port = (u16)std::atoi(argv[1]);
    std::memcpy(cfg.mac, mac.data(), 6);
    cfg.helloTo = argv[2];
    cfg.loopback = true;
    if (!UdsLink::Start(cfg, [&](const u8 src[6], const u8 dst[6], u8 type, u8 channel, const u8* data, size_t len) {
            Frame f;
            f.type = (FrameType)type;
            std::memcpy(f.src.data(), src, 6);
            std::memcpy(f.dst.data(), dst, 6);
            f.channel = channel;
            f.data.assign(data, data + len);
            Dump("RECEIVED", f);
            uds.OnFrame(f);
        }, [&] { return uds.Initialized(); }))
    {
        std::printf("link: couldn't open the port\n");
        return 1;
    }
    std::atomic<bool> running {true};
    std::thread ticker([&] { while (running) { uds.Tick(NowMs()); SleepMs(2); } });

    // udsInit: our node (a friend code seed and the user name)
    u8 node[kNodeInfoSize] = {};
    for (int i = 0; i < 8; i++) node[i] = (u8)rng();
    for (size_t i = 0; i < name.size() && i < 10; i++) node[8 + 2 * i] = (u8)name[i];
    std::printf("udsInit: %08x\n", uds.Initialize(node, 0x400));

    ScanResult found;
    bool have = false;
    if (mode != "host")
        for (int i = 0; i < (mode == "join" ? 30 : 3) && !have; i++)
        {
            have = Scan(uds, found);
            std::printf("scan %d: %u found\n", i, have ? 1u : 0u);
            if (!have) SleepMs(1000);
        }

    bool isHost = false;
    u32 ret;
    if (have)
    {
        std::printf("network of %02X:%02X:%02X:%02X:%02X:%02X, channel %u, %u/%u nodes, sha1 %s, host \"%s\" (%u in the node list)\n",
                    found.network[0], found.network[1], found.network[2], found.network[3], found.network[4], found.network[5],
                    found.network[6], found.network[0x1C], found.network[0x1D], found.shaOk ? "ok" : "BAD", found.hostName.c_str(), found.nodes);
        // udsConnectNetwork: connect (the request returns when the connection is made, or after 5 s), then bind
        uds.ConnectToNetwork(found.network, sizeof(found.network), 1);
        {
            std::unique_lock<std::mutex> l(host.m);
            ret = host.cv.wait_for(l, std::chrono::seconds(5), [&] { return host.connected; }) ? Result::Success : Result::Timeout;
        }
        if (ret == Result::Success) ret = uds.Bind(kBindId, 0x2E30, kChannel, 0xFFFF);
        std::printf("join: %08x\n", ret);
    }
    else if (mode == "join")
    {
        std::printf("RESULT FAIL (no network)\n");
        ret = 1;
    }
    else
    {
        isHost = true;
        // udsGenerateDefaultNetworkStruct + udsCreateNetwork
        u8 net[kNetworkInfoSize] = {};
        net[8] = 1;
        net[0xC] = 0x00; net[0xD] = 0x1F; net[0xE] = 0x32; net[0xF] = 21;
        net[0x10] = kWlanCommId >> 24; net[0x11] = (u8)(kWlanCommId >> 16); net[0x12] = (u8)(kWlanCommId >> 8); net[0x13] = (u8)kWlanCommId;
        net[0x14] = 0;
        const u32 networkId = rng();
        net[0x18] = networkId >> 24; net[0x19] = (u8)(networkId >> 16); net[0x1A] = (u8)(networkId >> 8); net[0x1B] = (u8)networkId;
        net[0x1D] = 16;
        ret = uds.BeginHostingNetwork(net, sizeof(net));
        if (ret == Result::Success) ret = uds.Bind(kBindId, 0x2E30, kChannel, 0xFFFF);
        std::printf("host: %08x (network id %08x)\n", ret, networkId);
    }

    int exitCode = 1;
    if (ret == Result::Success)
    {
        u32 sent = 0, received = 0, lastSeq = 0, errors = 0;
        u16 lastFrom = 0;
        const u64 start = NowMs();
        u64 nextFrame = start;
        for (int frame = 1; NowMs() - start < seconds * 1000; frame++)
        {
            nextFrame += 1000 / 60;
            while (NowMs() < nextFrame) SleepMs(1);
            if (frame % 10 == 0)
            {
                u8 msg[8];
                for (int i = 0; i < 4; i++) { msg[i] = (u8)(kMagic >> (8 * i)); msg[4 + i] = (u8)(sent >> (8 * i)); }
                const u32 r = uds.SendTo(0xFFFF, kChannel, msg, sizeof(msg), 1);
                if (r == Result::Success) sent++;
                else if (errors++ < 3) std::printf("send: %08x\n", r);
            }
            std::vector<u8> out;
            u32 got = 0;
            u16 from = 0;
            // udsPullPacket(64 bytes): the size, and the size aligned in words
            if (uds.PullPacket(kBindId, 64, 16, out, got, from) == Result::Success && got >= 8 && Le32(out.data()) == kMagic)
            {
                received++;
                lastSeq = Le32(out.data() + 4);
                lastFrom = from;
            }
        }
        u8 st[kConnectionStatusSize];
        uds.GetConnectionStatus(st);
        std::printf("%s: sent %u, received %u (last #%u from node %u), nodes %u, status %u, my node %u\n", isHost ? "host" : "client",
                    sent, received, lastSeq, lastFrom, st[44], Le32(st), Le16(st + 8));
        if (have)
        {
            u8 info[kNodeInfoSize];
            if (uds.GetNodeInformation(1, info) == Result::Success) std::printf("node 1 (host): \"%s\"\n", Utf16Name(info + 8).c_str());
        }
        else
            for (u16 n = 2; n <= 3; n++)
            {
                u8 info[kNodeInfoSize];
                if (uds.GetNodeInformation(n, info) == Result::Success) std::printf("node %u: \"%s\"\n", n, Utf16Name(info + 8).c_str());
            }
        std::printf(received > 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
        exitCode = received > 0 ? 0 : 1;
        if (vanish)
        {
            std::printf("(vanishing: no disconnect, no goodbye)\n");
            std::fflush(stdout);
            std::_Exit(exitCode);
        }
        if (isHost) uds.DestroyNetwork();
        else uds.DisconnectNetwork();
        uds.Unbind(kBindId);
    }
    else if (ret != 1)
        std::printf("RESULT FAIL\n");
    std::printf("connection events %u, data events %u\n", host.connectionEvents.load(), host.bindEvents.load());
    uds.Shutdown();
    SleepMs(200);
    running = false;
    ticker.join();
    UdsLink::Stop();
    return exitCode;
}
