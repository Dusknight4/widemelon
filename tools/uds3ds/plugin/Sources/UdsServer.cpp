// MultiMelon Wifi: the nwm::UDS server (see UdsServer.h). Requests are read as Azahar's NWM_UDS reads
// them (src/core/hle/service/nwm/nwm_uds.cpp) and answered as it answers them.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UdsServer.h"

#include <CTRPluginFramework.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "Log.h"
#include "uds_core.h"

namespace UdsServer
{
namespace
{
namespace UC = UdsCore;

constexpr Result kSessionClosed = (Result)0xC920181A;
constexpr u32 kSlotSize = 0x600;    // each static buffer we receive into (the largest: SendTo's 0x5C6)
constexpr int kSlots = 16;
constexpr u64 kConnectTimeoutNs = 5000000000ull;
constexpr u64 kBeaconScanNs = 300000000ull;

Link3ds::Config g_config;
LightLock g_lock;       // g_bindEvents (and the start-up below)
bool g_ready = false;
Handle g_connectionEvent = 0;   // the connection status event Initialize hands out
Handle g_connectedEvent = 0;    // ConnectToNetwork's: the connection has been made
std::map<u32, Handle> g_bindEvents;

struct Locked
{
    Locked() { LightLock_Lock(&g_lock); }
    ~Locked() { LightLock_Unlock(&g_lock); }
};

struct PluginHost final : UC::Host
{
    void Send(const UC::Frame& f) override { Link3ds::Send(f.dst.data(), (u8)f.type, f.channel, f.data.data(), f.data.size()); }
    void SignalConnectionEvent() override { if (g_connectionEvent) svcSignalEvent(g_connectionEvent); }
    void SignalBindEvent(u32 bindNodeId) override
    {
        Locked l;
        auto it = g_bindEvents.find(bindNodeId);
        if (it != g_bindEvents.end()) svcSignalEvent(it->second);
    }
    void SignalConnected() override { if (g_connectedEvent) svcSignalEvent(g_connectedEvent); }
};

PluginHost g_host;
UC::Service* g_uds = nullptr;

void Setup()
{
    if (g_ready) return;
    LightLock_Init(&g_lock);
    svcCreateEvent(&g_connectionEvent, RESET_ONESHOT);
    svcCreateEvent(&g_connectedEvent, RESET_ONESHOT);
    UC::Mac mac;
    std::memcpy(mac.data(), g_config.mac, 6);
    g_uds = new UC::Service(g_host, mac);
    g_ready = true;
}

void StartLink()
{
    Link3ds::Start(
        g_config,
        [](const u8 src[6], const u8 dst[6], u8 type, u8 channel, const u8* data, size_t len) {
            UC::Frame f;
            f.type = (UC::FrameType)type;
            std::memcpy(f.src.data(), src, 6);
            std::memcpy(f.dst.data(), dst, 6);
            f.channel = channel;
            f.data.assign(data, data + len);
            g_uds->OnFrame(f);
        },
        [] { return g_uds->Initialized(); }, [](u64 now) { g_uds->Tick(now); });
}

// a request's translated parameters, in order
struct Buffer { u32 size; u8* ptr; u32 desc; };
struct Parsed
{
    std::vector<Buffer> statics, mapped;
    std::vector<Handle> handles;
};

Parsed Parse(const u32* req)
{
    Parsed p;
    const u32 normal = (req[0] >> 6) & 0x3F, translate = req[0] & 0x3F;
    for (u32 i = 1 + normal; i < 1 + normal + translate && i < 64;)
    {
        const u32 d = req[i];
        if ((d & 0xF) == 0x0)       // handles (or the process id)
        {
            const u32 n = (d & 0x20) ? 1 : (d >> 26) + 1;
            for (u32 k = 0; k < n && i + 1 + k < 64; k++)
                if (!(d & 0x20)) p.handles.push_back(req[i + 1 + k]);
            i += 1 + n;
        }
        else if ((d & 0xF) == 0x2)  // static buffer
        {
            p.statics.push_back({d >> 14, (u8*)req[i + 1], d});
            i += 2;
        }
        else if (d & 0x8)           // mapped buffer
        {
            p.mapped.push_back({d >> 4, (u8*)req[i + 1], d});
            i += 2;
        }
        else
            i += 2;
    }
    return p;
}

// a reply: the header and the words after it
struct Reply
{
    u32 w[64] {};
    u32 n = 0;
    void Header(u32 cmd, u32 normal, u32 translate)
    {
        w[0] = IPC_MakeHeader(cmd, normal, translate);
        n = 1;
    }
    void Push(u32 v) { w[n++] = v; }
};

struct Session
{
    Handle server = 0;
    u8 slots[kSlots][kSlotSize] __attribute__((aligned(8)));
    u8 out[kSlotSize] __attribute__((aligned(8)));   // a reply's static buffer
};

const char* Name(u32 cmd)
{
    switch (cmd)
    {
    case 0x01: return "Initialize (deprecated)";
    case 0x03: return "Shutdown";
    case 0x04: return "BeginHostingNetwork (deprecated)";
    case 0x05: return "EjectClient";
    case 0x06: return "EjectSpectators";
    case 0x07: return "UpdateNetworkAttribute";
    case 0x08: return "DestroyNetwork";
    case 0x09: return "ConnectToNetwork (deprecated)";
    case 0x0A: return "DisconnectNetwork";
    case 0x0B: return "GetConnectionStatus";
    case 0x0D: return "GetNodeInformation";
    case 0x0E: return "DecryptBeaconData (deprecated)";
    case 0x0F: return "RecvBeaconBroadcastData";
    case 0x10: return "SetApplicationData";
    case 0x11: return "GetApplicationData";
    case 0x12: return "Bind";
    case 0x13: return "Unbind";
    case 0x14: return "PullPacket";
    case 0x15: return "SetMaxSendDelay";
    case 0x17: return "SendTo";
    case 0x1A: return "GetChannel";
    case 0x1B: return "InitializeWithVersion";
    case 0x1D: return "BeginHostingNetwork";
    case 0x1E: return "ConnectToNetwork";
    case 0x1F: return "DecryptBeaconData";
    default: return "?";
    }
}

// frequent ones are logged a few times, then every 500th
bool ShouldLog(u32 cmd, u32 result)
{
    static unsigned counts[0x40] = {};
    const unsigned c = ++counts[cmd & 0x3F];
    const bool frequent = cmd == 0x0B || cmd == 0x0D || cmd == 0x0F || cmd == 0x14 || cmd == 0x17 || cmd == 0x1A;
    if (!frequent) return true;
    if (result != 0 && cmd != 0x14) return c <= 20 || c % 100 == 0;
    return c <= 3 || c % 500 == 0;
}

u32 Initialize(const u32* req, const Parsed& p, Reply& r, u32 cmd, u16 version)
{
    for (Handle h : p.handles) svcCloseHandle(h);     // (the receive buffer's shared memory: unused, as in Azahar)
    const u32 result = g_uds->Initialize((const u8*)&req[2], version);
    StartLink();
    r.Header(cmd, 1, 2);
    r.Push(result);
    r.Push(IPC_Desc_SharedHandles(1));
    r.Push(g_connectionEvent);
    return result;
}

u32 Connect(const u8* info, size_t size, u8 type, Reply& r, u32 cmd)
{
    svcClearEvent(g_connectedEvent);
    g_uds->ConnectToNetwork(info, size, type);
    const Result w = svcWaitSynchronization(g_connectedEvent, kConnectTimeoutNs);
    const u32 result = (w == 0 && g_uds->ConnectedAsClientOrSpectator()) ? UC::Result::Success : UC::Result::Timeout;
    r.Header(cmd, 1, 0);
    r.Push(result);
    return result;
}

u32 Dispatch(Session& s, const u32* req, Reply& r)
{
    const u32 cmd = req[0] >> 16;
    const Parsed p = Parse(req);
    auto simple = [&](u32 result) {
        r.Header(cmd, 1, 0);
        r.Push(result);
        return result;
    };
    switch (cmd)
    {
    case 0x1B: return Initialize(req, p, r, cmd, (u16)req[12]);
    case 0x01: return Initialize(req, p, r, cmd, 0x100);
    case 0x03:
    {
        g_uds->Shutdown();
        Locked l;
        for (auto& e : g_bindEvents) svcCloseHandle(e.second);
        g_bindEvents.clear();
        return simple(UC::Result::Success);
    }
    case 0x1D:
        if (p.statics.empty()) return simple(UC::Result::NotAuthorizedWrongArg);
        return simple(g_uds->BeginHostingNetwork(p.statics[0].ptr, std::min<u32>(p.statics[0].size, UC::kNetworkInfoSize)));
    case 0x04: return simple(g_uds->BeginHostingNetwork((const u8*)&req[1], 0x3C));
    case 0x05: return simple(g_uds->EjectClient((u16)req[1]));
    case 0x06: return simple(UC::Result::Success);
    case 0x07: return simple(g_uds->UpdateNetworkAttribute((u16)req[1], (u8)req[2]));
    case 0x08: return simple(g_uds->DestroyNetwork());
    case 0x1E:
        if (p.statics.empty()) return simple(UC::Result::NotAuthorizedWrongArg);
        return Connect(p.statics[0].ptr, std::min<u32>(p.statics[0].size, UC::kNetworkInfoSize), (u8)req[1], r, cmd);
    case 0x09: return Connect((const u8*)&req[1], 0x3C, (u8)req[16], r, cmd);
    case 0x0A: return simple(g_uds->DisconnectNetwork());
    case 0x0B:
    {
        r.Header(cmd, 13, 0);
        r.Push(UC::Result::Success);
        g_uds->GetConnectionStatus((u8*)&r.w[2]);
        r.n = 14;
        return UC::Result::Success;
    }
    case 0x0D:
    {
        u8 node[UC::kNodeInfoSize];
        const u32 result = g_uds->GetNodeInformation((u16)req[1], node);
        if (result != UC::Result::Success) return simple(result);
        r.Header(cmd, 11, 0);
        r.Push(result);
        std::memcpy(&r.w[2], node, UC::kNodeInfoSize);
        r.n = 12;
        return result;
    }
    case 0x1F:
    case 0x0E:
    {
        if (p.statics.size() < 3) return simple(UC::Result::NotAuthorizedWrongArg);
        const u32 result = g_uds->DecryptBeaconData(p.statics[0].ptr, p.statics[1].ptr, p.statics[1].size, p.statics[2].ptr,
                                                    p.statics[2].size, s.out);
        r.Header(cmd, 1, 2);
        r.Push(result);
        r.Push(IPC_Desc_StaticBuffer(UC::kNodeInfoSize * UC::kMaxNodes, 0));
        r.Push((u32)s.out);
        return result;
    }
    case 0x0F:
    {
        for (Handle h : p.handles) svcCloseHandle(h);     // (the scan's event: unused, as in Azahar)
        if (p.mapped.empty()) return simple(UC::Result::NotAuthorizedWrongArg);
        UC::Mac sender;
        std::memcpy(sender.data(), &req[4], 6);
        const u32 result = g_uds->RecvBeaconBroadcastData(sender, p.mapped[0].ptr, std::min(req[1], p.mapped[0].size));
        svcSleepThread(kBeaconScanNs);
        r.Header(cmd, 1, 2);
        r.Push(result);
        r.Push(p.mapped[0].desc);
        r.Push((u32)p.mapped[0].ptr);
        return result;
    }
    case 0x10:
        if (p.statics.empty()) return simple(UC::Result::NotAuthorizedWrongArg);
        return simple(g_uds->SetApplicationData(p.statics[0].ptr, std::min(req[1], p.statics[0].size)));
    case 0x11:
    {
        std::vector<u8> data;
        const u32 result = g_uds->GetApplicationData(req[1], data);
        if (data.empty())
        {
            r.Header(cmd, 2, 0);
            r.Push(result);
            r.Push(0);
            return result;
        }
        std::memcpy(s.out, data.data(), data.size());
        r.Header(cmd, 2, 2);
        r.Push(result);
        r.Push(data.size());
        r.Push(IPC_Desc_StaticBuffer(data.size(), 0));
        r.Push((u32)s.out);
        return result;
    }
    case 0x12:
    {
        const u32 bindNodeId = req[1];
        Handle event = 0;
        svcCreateEvent(&event, RESET_ONESHOT);
        {
            Locked l;
            auto it = g_bindEvents.find(bindNodeId);
            if (it != g_bindEvents.end()) svcCloseHandle(it->second);
            g_bindEvents[bindNodeId] = event;
        }
        const u32 result = g_uds->Bind(bindNodeId, req[2], (u8)req[3], (u16)req[4]);
        if (result != UC::Result::Success)
        {
            Locked l;
            g_bindEvents.erase(bindNodeId);
            svcCloseHandle(event);
            return simple(result);
        }
        r.Header(cmd, 1, 2);
        r.Push(result);
        r.Push(IPC_Desc_SharedHandles(1));
        r.Push(event);
        return result;
    }
    case 0x13:
    {
        const u32 bindNodeId = req[1];
        if (bindNodeId == 0) return simple(UC::Result::NotAuthorizedWrongArg);
        g_uds->Unbind(bindNodeId);
        {
            Locked l;
            auto it = g_bindEvents.find(bindNodeId);
            if (it != g_bindEvents.end())
            {
                svcCloseHandle(it->second);
                g_bindEvents.erase(it);
            }
        }
        r.Header(cmd, 5, 0);
        r.Push(UC::Result::Success);
        r.Push(bindNodeId);
        r.Push(0);
        r.Push(0);
        r.Push(0);
        return UC::Result::Success;
    }
    case 0x14:
    {
        std::vector<u8> out;
        u32 size = 0;
        u16 src = 0;
        const u32 result = g_uds->PullPacket(req[1], req[3], req[2], out, size, src);
        if (result != UC::Result::Success) return simple(result);
        const u32 n = std::min<u32>(out.size(), kSlotSize);
        std::memcpy(s.out, out.data(), n);
        r.Header(cmd, 3, 2);
        r.Push(result);
        r.Push(size);
        r.Push(src);
        r.Push(IPC_Desc_StaticBuffer(n, 0));
        r.Push((u32)s.out);
        return result;
    }
    case 0x17:
    {
        if (p.statics.empty()) return simple(UC::Result::NotAuthorizedWrongArg);
        const u32 size = std::min(req[5], p.statics[0].size);
        return simple(g_uds->SendTo((u16)req[2], (u8)req[3], p.statics[0].ptr, size, (u8)req[6]));
    }
    case 0x1A:
    {
        r.Header(cmd, 2, 0);
        r.Push(UC::Result::Success);
        r.Push(g_uds->GetChannel());
        return UC::Result::Success;
    }
    default:
        for (Handle h : p.handles) svcCloseHandle(h);
        return simple(UC::Result::Success);
    }
}

void SetReceiveBuffers(Session& s)
{
    u32* sb = getThreadStaticBuffers();
    for (int i = 0; i < kSlots; i++)
    {
        sb[2 * i] = IPC_Desc_StaticBuffer(kSlotSize, i);
        sb[2 * i + 1] = (u32)s.slots[i];
    }
}

void Serve(void* arg)
{
    Session& s = *(Session*)arg;
    Handle target = 0;
    unsigned served = 0;
    while (true)
    {
        SetReceiveBuffers(s);
        s32 index = -1;
        const Result rr = svcReplyAndReceive(&index, &s.server, 1, target);
        if (rr == kSessionClosed) break;
        if (R_FAILED(rr))
        {
            LogF("uds: session %08lX: ReplyAndReceive %08lX\n", (u32)s.server, rr);
            target = 0;
            svcSleepThread(1000000);    // (1 ms: no busy loop if this keeps failing)
            continue;
        }
        // the request out of the command buffer first: anything below may make requests of its own
        u32 req[64];
        std::memcpy(req, getThreadCommandBuffer(), sizeof(req));
        Reply r;
        const u32 cmd = req[0] >> 16;
        const u32 result = Dispatch(s, req, r);
        if (ShouldLog(cmd, result)) LogF("uds: %s (%02lX) -> %08lX\n", Name(cmd), cmd, result);
        std::memcpy(getThreadCommandBuffer(), r.w, r.n * 4);
        target = s.server;
        served++;
    }
    LogF("uds: session %08lX closed after %u requests\n", (u32)s.server, served);
    svcCloseHandle(s.server);
    delete &s;
}

}

void Configure(const Link3ds::Config& config) { g_config = config; }

Result OpenSession(Handle* client)
{
    Setup();
    Session* s = new Session;
    Handle c = 0;
    Result r = svcCreateSession(&s->server, &c);
    if (R_FAILED(r))
    {
        LogF("uds: CreateSession %08lX\n", r);
        delete s;
        return r;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (!threadCreate(Serve, s, 0x4000, prio > 0x18 ? 0x18 : prio - 1, -2, true))
    {
        LogF("uds: no server thread\n");
        svcCloseHandle(s->server);
        svcCloseHandle(c);
        delete s;
        return -1;
    }
    *client = c;
    LogF("uds: session %08lX for the game (client %08lX)\n", (u32)s->server, (u32)c);
    return 0;
}

namespace
{
namespace CTRPF = CTRPluginFramework;

Result HookedGetServiceHandle(Handle* out, const char* name, s32 len, u32 flags)
{
    // (the SDK passes the name's length; libctru's srvGetServiceHandleDirect has a C string and no length)
    if (CTRPF::Process::CheckAddress((u32)name, MEMPERM_READ) && std::memcmp(name, "nwm::UDS", 8) == 0 && (len == 8 || name[8] == 0) &&
        CTRPF::Process::CheckAddress((u32)out))
    {
        if (R_SUCCEEDED(OpenSession(out))) return 0;
        LogF("uds: falling back to the console's nwm::UDS\n");
    }
    return CTRPF::HookContext::GetCurrent().OriginalFunction<Result>(out, name, len, flags);
}

CTRPF::Hook g_hooks[4];
}

int InstallHooks()
{
    const u32 base = 0x100000;
    const u32 words = CTRPF::Process::GetTextSize() / 4;
    const u32* text = (const u32*)base;
    int hooked = 0;
    for (u32 i = 0; i < words; i++)
    {
        if (text[i] != 0x00050100) continue;     // srv GetServiceHandle's request header, in a literal pool
        const u32 literal = base + i * 4;
        for (u32 j = i > 64 ? i - 64 : 0; j < i; j++)
        {
            const u32 ins = text[j];
            if ((ins & 0xFFFF0000) != 0xE59F0000 || base + j * 4 + 8 + (ins & 0xFFF) != literal) continue;    // ldr rX, =header
            bool svc = false;   // svcSendSyncRequest after it: inline (the SDK), or a bl to a wrapper (libctru)
            for (u32 k = j; k < i; k++)
            {
                if (text[k] == 0xEF000032) svc = true;
                else if ((text[k] >> 24) == 0xEB)
                {
                    const u32 target = base + k * 4 + 8 + ((u32)((s32)(text[k] << 8) >> 6));
                    if (target >= base && target < base + words * 4 && text[(target - base) / 4] == 0xEF000032) svc = true;
                }
            }
            if (!svc) continue;
            u32 start = 0;      // the function's push {..., lr}
            for (u32 k = j; k > 0 && k + 24 > j; k--)
                if ((text[k] & 0xFFFF4000) == 0xE92D4000) { start = base + k * 4; break; }
            if (!start || hooked >= 4) continue;
            const CTRPF::HookResult hr = g_hooks[hooked].InitializeForMitm(start, (u32)HookedGetServiceHandle).Enable();
            LogF("hook: srv GetServiceHandle at %08lX (request at %08lX): %d\n", start, base + j * 4, (int)hr);
            if (hr == CTRPF::HookResult::Success) hooked++;
        }
    }
    if (!hooked) LogF("hook: srv GetServiceHandle not found\n");
    return hooked;
}

}
