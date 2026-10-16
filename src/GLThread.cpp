/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a fork of melonDS.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#include "GLThread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <immintrin.h>

#include "OpenGLSupport.h"

namespace melonDS::GLThread
{
namespace
{

// The queue: a ring of records, each a header, a callable and the data it
// needs copied. One thread writes (whichever holds GL), the render thread reads.
constexpr size_t kCapacity = size_t(64) << 20;
constexpr size_t kMaxCopy = size_t(16) << 20;    // bigger data: the call waits instead

struct alignas(16) Header
{
    void (*Exec)(Header*);      // nullptr: the rest of the ring is skipped
    u32 Size;
    u32 Pad;
};
static_assert(sizeof(Header) == 16);

u8* Ring = nullptr;
std::atomic<u64> WritePub {0};      // what the render thread may read up to
std::atomic<u64> ReadPos {0};       // what the render thread has finished
u64 WriteLocal = 0;                 // the writer's own position

std::thread Thread;
std::atomic<bool> Active {false};
std::atomic<bool> StopRequested {false};
thread_local bool IsRender = false;

std::mutex WakeMutex;
std::condition_variable WakeConsumer, WakeProducer;
std::atomic<bool> ConsumerSleeping {false};
std::atomic<bool> ProducerWaiting {false};

std::atomic<u64> SwapsDone {0};
u64 SwapsQueued = 0;
std::atomic<unsigned long long> Syncs {0};

inline bool OnRenderThread() { return IsRender; }
inline size_t AlignUp(size_t v) { return (v + 15) & ~size_t(15); }

void WakeRender()
{
    if (ConsumerSleeping.load())
    {
        std::lock_guard lock(WakeMutex);
        WakeConsumer.notify_one();
    }
}

// the writer waits for the render thread (spinning briefly first)
template <typename C>
void WaitFor(const C& cond)
{
    for (int i = 0; i < 4000; i++)
    {
        if (cond()) return;
        _mm_pause();
    }
    WakeRender();
    std::unique_lock lock(WakeMutex);
    ProducerWaiting = true;
    while (!cond())
        WakeProducer.wait_for(lock, std::chrono::microseconds(200));
    ProducerWaiting = false;
}

void WaitSpace(size_t bytes)
{
    WaitFor([bytes] { return WriteLocal + bytes - ReadPos.load(std::memory_order_acquire) <= kCapacity; });
}

u8* Reserve(size_t total)
{
    const size_t pos = WriteLocal % kCapacity;
    if (pos + total > kCapacity)
    {
        // not enough room before the end: mark the rest as skipped
        WaitSpace(kCapacity - pos);
        Header* h = reinterpret_cast<Header*>(Ring + pos);
        h->Exec = nullptr;
        h->Size = 0;
        WriteLocal += kCapacity - pos;
    }
    WaitSpace(total);
    return Ring + (WriteLocal % kCapacity);
}

void Commit(size_t total)
{
    WriteLocal += total;
    WritePub.store(WriteLocal);
    WakeRender();
}

template <typename F>
void ExecRecord(Header* h)
{
    F* f = reinterpret_cast<F*>(h + 1);
    const u8* data = reinterpret_cast<const u8*>(f) + AlignUp(sizeof(F));
    (*f)(static_cast<const void*>(data));
    f->~F();
}

// queues `fn(data)`, with `len` bytes from `src` copied in as its data
template <typename F>
void Enqueue(const F& fn, const void* src = nullptr, size_t len = 0)
{
    static_assert(alignof(F) <= 16);
    const size_t fsize = AlignUp(sizeof(F));
    const size_t total = sizeof(Header) + fsize + AlignUp(len);
    u8* p = Reserve(total);
    Header* h = reinterpret_cast<Header*>(p);
    h->Exec = &ExecRecord<F>;
    h->Size = (u32)total;
    new (p + sizeof(Header)) F(fn);
    if (len) memcpy(p + sizeof(Header) + fsize, src, len);
    Commit(total);
}

void SyncImpl()
{
    const u64 target = WriteLocal;
    WaitFor([target] { return ReadPos.load(std::memory_order_acquire) >= target; });
}

// runs fn on the render thread and waits for its result
template <typename F>
auto Call(const F& fn) -> decltype(fn())
{
    using R = decltype(fn());
    Syncs++;
    if constexpr (std::is_void_v<R>)
    {
        Enqueue([&fn](const void*) { fn(); });
        SyncImpl();
    }
    else
    {
        R result {};
        Enqueue([&fn, &result](const void*) { result = fn(); });
        SyncImpl();
        return result;
    }
}

void RenderMain(std::function<void()> makeCurrent, std::function<void()> doneCurrent, std::atomic<bool>* ready)
{
    IsRender = true;
    makeCurrent();
    ready->store(true);

    u64 r = ReadPos.load();
    for (;;)
    {
        const u64 w = WritePub.load(std::memory_order_acquire);
        if (r == w)
        {
            if (StopRequested.load()) break;
            bool more = false;
            for (int i = 0; i < 2000 && !more; i++)
            {
                _mm_pause();
                more = WritePub.load(std::memory_order_acquire) != r || StopRequested.load();
            }
            if (!more)
            {
                std::unique_lock lock(WakeMutex);
                ConsumerSleeping = true;
                while (WritePub.load() == r && !StopRequested.load())
                    WakeConsumer.wait_for(lock, std::chrono::milliseconds(1));
                ConsumerSleeping = false;
            }
            continue;
        }
        while (r != w)
        {
            Header* h = reinterpret_cast<Header*>(Ring + (r % kCapacity));
            if (!h->Exec)
                r = (r / kCapacity + 1) * kCapacity;
            else
            {
                const u32 size = h->Size;
                h->Exec(h);
                r += size;
            }
        }
        ReadPos.store(r, std::memory_order_release);
        if (ProducerWaiting.load())
        {
            std::lock_guard lock(WakeMutex);
            WakeProducer.notify_all();
        }
    }
    doneCurrent();
}

// bytes glTex(Sub)Image reads from `pixels` (rows aligned to 4, the unpack
// state WideMelon never changes)
size_t TexBytes(const void* pixels, GLenum format, GLenum type, GLsizei w, GLsizei h, GLsizei d)
{
    if (!pixels || w <= 0 || h <= 0 || d <= 0) return 0;
    size_t bpp;
    switch (type)
    {
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        bpp = 2; break;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
        bpp = 4; break;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        bpp = 8; break;
    default:
    {
        size_t comps;
        switch (format)
        {
        case GL_RG: case GL_RG_INTEGER: comps = 2; break;
        case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER: comps = 3; break;
        case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: comps = 4; break;
        default: comps = 1; break;
        }
        size_t size;
        switch (type)
        {
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: size = 2; break;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: size = 4; break;
        default: size = 1; break;
        }
        bpp = comps * size;
    }
    }
    const size_t row = (w * bpp + 3) & ~size_t(3);
    return row * ((size_t)h * d - 1) + w * bpp;
}

size_t ClearBufferBytes(GLenum buffer, size_t elem)
{
    return (buffer == GL_COLOR ? 4 : 1) * elem;
}

// state kept on the writer's side, for calls that would otherwise have to wait
std::mutex CacheMutex;
std::unordered_map<GLuint, std::unordered_map<std::string, GLint>> UniformLocations;
GLuint PackBuffer = 0;
GLuint ActiveUnit = 0;
std::map<std::pair<GLuint, GLenum>, GLuint> BoundTextures;     // known since Start()
std::map<std::tuple<GLuint, GLenum, GLint, GLenum>, GLint> TexLevelParams;

void OnTexChange()
{
    std::lock_guard lock(CacheMutex);
    TexLevelParams.clear();
}

}   // namespace

#include "GLThreadStubs.inc"

namespace
{

// hand-written stubs (see CUSTOM in the generator)

GLint APIENTRY stub_glGetUniformLocation(GLuint program, const GLchar* name)
{
    if (OnRenderThread()) return real_glGetUniformLocation(program, name);
    {
        std::lock_guard lock(CacheMutex);
        auto it = UniformLocations.find(program);
        if (it != UniformLocations.end())
        {
            auto loc = it->second.find(name);
            if (loc != it->second.end()) return loc->second;
        }
    }
    const GLint loc = Call([&] { return real_glGetUniformLocation(program, name); });
    std::lock_guard lock(CacheMutex);
    UniformLocations[program][name] = loc;
    return loc;
}

void APIENTRY stub_glLinkProgram(GLuint program)
{
    {
        std::lock_guard lock(CacheMutex);
        UniformLocations.erase(program);
    }
    if (OnRenderThread()) { real_glLinkProgram(program); return; }
    Enqueue([=](const void*) { real_glLinkProgram(program); });
}

void APIENTRY stub_glDeleteProgram(GLuint program)
{
    {
        std::lock_guard lock(CacheMutex);
        UniformLocations.erase(program);
    }
    if (OnRenderThread()) { real_glDeleteProgram(program); return; }
    Enqueue([=](const void*) { real_glDeleteProgram(program); });
}

void APIENTRY stub_glBindBuffer(GLenum target, GLuint buffer)
{
    if (target == GL_PIXEL_PACK_BUFFER) PackBuffer = buffer;
    if (OnRenderThread()) { real_glBindBuffer(target, buffer); return; }
    Enqueue([=](const void*) { real_glBindBuffer(target, buffer); });
}

void APIENTRY stub_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    if (OnRenderThread()) { real_glReadPixels(x, y, width, height, format, type, pixels); return; }
    if (PackBuffer)     // into a buffer: `pixels` is an offset, nothing to wait for
        Enqueue([=](const void*) { real_glReadPixels(x, y, width, height, format, type, pixels); });
    else
        Call([&] { real_glReadPixels(x, y, width, height, format, type, pixels); });
}

void APIENTRY stub_glActiveTexture(GLenum texture)
{
    ActiveUnit = texture - GL_TEXTURE0;
    if (OnRenderThread()) { real_glActiveTexture(texture); return; }
    Enqueue([=](const void*) { real_glActiveTexture(texture); });
}

void APIENTRY stub_glBindTexture(GLenum target, GLuint texture)
{
    {
        std::lock_guard lock(CacheMutex);
        BoundTextures[{ActiveUnit, target}] = texture;
    }
    if (OnRenderThread()) { real_glBindTexture(target, texture); return; }
    Enqueue([=](const void*) { real_glBindTexture(target, texture); });
}

void APIENTRY stub_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params)
{
    if (OnRenderThread()) { real_glGetTexLevelParameteriv(target, level, pname, params); return; }
    // texture sizes don't change until the texture is (re)specified (OnTexChange)
    GLuint texture = 0;
    bool known = false;
    {
        std::lock_guard lock(CacheMutex);
        auto bound = BoundTextures.find({ActiveUnit, target});
        if (bound != BoundTextures.end())
        {
            known = true;
            texture = bound->second;
            auto it = TexLevelParams.find({texture, target, level, pname});
            if (it != TexLevelParams.end()) { *params = it->second; return; }
        }
    }
    Call([&] { real_glGetTexLevelParameteriv(target, level, pname, params); });
    if (known)
    {
        std::lock_guard lock(CacheMutex);
        TexLevelParams[{texture, target, level, pname}] = *params;
    }
}

void APIENTRY stub_glFinish()
{
    if (OnRenderThread()) { real_glFinish(); return; }
    Call([] { real_glFinish(); });
}

void InstallCustomStubs()
{
    glad_glGetUniformLocation = stub_glGetUniformLocation;
    glad_glLinkProgram = stub_glLinkProgram;
    glad_glDeleteProgram = stub_glDeleteProgram;
    glad_glBindBuffer = stub_glBindBuffer;
    glad_glReadPixels = stub_glReadPixels;
    glad_glActiveTexture = stub_glActiveTexture;
    glad_glBindTexture = stub_glBindTexture;
    glad_glGetTexLevelParameteriv = stub_glGetTexLevelParameteriv;
    glad_glFinish = stub_glFinish;
}

}   // namespace

void Start(std::function<void()> makeCurrent, std::function<void()> doneCurrent)
{
    if (Active) return;
    if (!Ring)
    {
        Ring = static_cast<u8*>(std::malloc(kCapacity));   // 16-byte aligned, enough for the records
        if (!Ring) return;
    }
    WritePub = 0;
    ReadPos = 0;
    WriteLocal = 0;
    SwapsDone = 0;
    SwapsQueued = 0;
    StopRequested = false;
    {
        std::lock_guard lock(CacheMutex);
        UniformLocations.clear();
        BoundTextures.clear();
        TexLevelParams.clear();
        PackBuffer = 0;
        ActiveUnit = 0;
    }

    std::atomic<bool> ready {false};
    Thread = std::thread(RenderMain, std::move(makeCurrent), std::move(doneCurrent), &ready);
    while (!ready.load()) std::this_thread::yield();

    InstallStubs();
    InstallCustomStubs();
    Active = true;
}

void Stop()
{
    if (!Active) return;
    SyncImpl();
    RemoveStubs();
    Active = false;
    StopRequested = true;
    {
        std::lock_guard lock(WakeMutex);
        WakeConsumer.notify_one();
    }
    Thread.join();
}

bool Running()
{
    return Active;
}

void Sync()
{
    if (Active && !OnRenderThread()) SyncImpl();
}

void Release()
{
    Sync();
}

void Swap(std::function<void()> swap, int maxPending)
{
    if (!Active || OnRenderThread())
    {
        swap();
        return;
    }
    Enqueue([swap](const void*)
    {
        swap();
        SwapsDone++;
        if (ProducerWaiting.load())
        {
            std::lock_guard lock(WakeMutex);
            WakeProducer.notify_all();
        }
    });
    SwapsQueued++;
    WaitFor([maxPending] { return SwapsQueued - SwapsDone.load() <= (u64)maxPending; });
}

void RunSync(const std::function<void()>& fn)
{
    if (!Active || OnRenderThread())
    {
        fn();
        return;
    }
    Call([&fn] { fn(); });
}

unsigned long long SyncCount()
{
    return Syncs.load();
}

}
