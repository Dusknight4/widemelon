// linkcheck: measures what UdsLink's rough link (UdsLink::Impairment) really does, with two processes on
// 127.0.0.1. "echo" sends every frame straight back; "ping" sends numbered frames through the rough link and
// reports what came back: lost, burst lengths, round-trip times, out of order, doubled.
//
//   linkcheck echo PORT OTHERPORT
//   linkcheck ping PORT OTHERPORT SECONDS [loss=.. burst=.. delay=.. jitter=.. dup=.. reorder=1 seed=..]
// build: build_linkcheck.sh
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "UdsLink.h"

namespace
{
double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::mutex g_lock;
std::map<uint32_t, int> g_got;          // seq -> times received
std::vector<double> g_rtt;
uint32_t g_maxSeq = 0;
unsigned g_outOfOrder = 0;
uint8_t g_peerMac[6] {};
std::atomic<bool> g_peerKnown {false};
}

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);   // (winmm, as the rig)
#endif

int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: linkcheck echo|ping PORT OTHERPORT [SECONDS] [key=value...]\n"); return 2; }
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    const bool ping = std::string(argv[1]) == "ping";
    UdsLink::Config cfg;
    cfg.port = (uint16_t)std::atoi(argv[2]);
    cfg.loopback = true;
    cfg.helloTo = std::string("127.0.0.1:") + argv[3];
    const uint8_t mac[6] = {0x02, 0x4C, 0x43, 0, 0, (uint8_t)(ping ? 1 : 2)};
    std::memcpy(cfg.mac, mac, 6);
    std::memcpy(g_peerMac, mac, 6);
    g_peerMac[5] = ping ? 2 : 1;

    auto onFrame = [&](const uint8_t src[6], const uint8_t*, uint8_t type, uint8_t channel, const uint8_t* data, size_t len) {
        g_peerKnown = true;
        if (!ping)
        {
            UdsLink::Send(src, type, channel, data, len);
            return;
        }
        if (len < 12 || !data[0] && !data[1] && !data[2] && !data[3]) return;   // (seq 0: the start-up probe)
        uint32_t seq;
        double sent;
        std::memcpy(&seq, data, 4);
        std::memcpy(&sent, data + 4, 8);
        std::lock_guard l(g_lock);
        if (++g_got[seq] > 1) return;   // (a doubled one: counted, no time)
        g_rtt.push_back(NowMs() - sent);
        if (seq < g_maxSeq) g_outOfOrder++;
        g_maxSeq = std::max(g_maxSeq, seq);
    };
    if (!UdsLink::Start(cfg, onFrame, [] { return true; })) { std::printf("no port\n"); return 1; }

    if (!ping)
    {
        std::this_thread::sleep_for(std::chrono::seconds(600));
        return 0;
    }
    const double seconds = argc > 4 ? std::atof(argv[4]) : 10.0;
    UdsLink::Impairment imp;
    for (int i = 5; i < argc; i++)
    {
        const std::string a = argv[i];
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = a.substr(0, eq);
        const double v = std::atof(a.c_str() + eq + 1);
        if (k == "loss") imp.lossPct = v;
        else if (k == "burst") imp.burst = v;
        else if (k == "delay") imp.delayMs = v;
        else if (k == "jitter") imp.jitterMs = v;
        else if (k == "dup") imp.dupPct = v;
        else if (k == "reorder") imp.reorder = v != 0;
        else if (k == "seed") imp.seed = (uint32_t)v;
    }
    // (wait until the echo has been heard: a clean probe, before the rough link goes on)
    const double waitUntil = NowMs() + 5000;
    while (!g_peerKnown && NowMs() < waitUntil)
    {
        uint8_t probe[12] {};
        UdsLink::Send(g_peerMac, 0, 1, probe, sizeof(probe));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!g_peerKnown) { std::printf("echo not heard\n"); return 1; }
    UdsLink::SetImpairment(imp);
    const auto before = UdsLink::GetStats();

    uint32_t seq = 1;
    const double end = NowMs() + seconds * 1000.0;
    while (NowMs() < end)
    {
        uint8_t msg[64] {};
        const double t = NowMs();
        std::memcpy(msg, &seq, 4);
        std::memcpy(msg + 4, &t, 8);
        UdsLink::Send(g_peerMac, 0, 1, msg, sizeof(msg));
        seq++;
        const double next = t + 1.0;    // (one a millisecond)
        while (NowMs() < next) std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(imp.delayMs + imp.jitterMs) + 500));
    const auto after = UdsLink::GetStats();
    UdsLink::Stop();

    const uint32_t sent = seq - 1;
    std::lock_guard l(g_lock);
    unsigned got = 0, doubled = 0, bursts = 0, lostRun = 0, lostTotal = 0;
    for (uint32_t s = 1; s <= sent; s++)
    {
        auto it = g_got.find(s);
        if (it == g_got.end())
        {
            lostTotal++;
            if (lostRun++ == 0) bursts++;
            continue;
        }
        lostRun = 0;
        got++;
        if (it->second > 1) doubled++;
    }
    std::sort(g_rtt.begin(), g_rtt.end());
    auto pct = [&](double p) { return g_rtt.empty() ? 0.0 : g_rtt[(size_t)(p * (g_rtt.size() - 1))]; };
    std::printf("sent %u frames, back %u (lost %.1f%%), loss bursts %u (average %.2f frames), doubled %u (%.1f%%), "
                "out of order %u\n",
                sent, got, 100.0 * lostTotal / sent, bursts, bursts ? (double)lostTotal / bursts : 0.0, doubled,
                100.0 * doubled / std::max(got, 1u), g_outOfOrder);
    std::printf("round trip ms: min %.1f, median %.1f, 95%% %.1f, max %.1f\n", pct(0), pct(0.5), pct(0.95), pct(1));
    std::printf("link counters: dropped %u, doubled %u (hellos included in dropped)\n", after.dropped - before.dropped,
                after.doubled - before.doubled);
    return 0;
}
