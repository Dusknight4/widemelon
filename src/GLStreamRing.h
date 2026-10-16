/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a fork of melonDS.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#pragma once

#include <cstring>
#include "OpenGLSupport.h"
#include "GLThread.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace melonDS
{

// A uniform buffer that small per-pass blocks of data are streamed into, for
// renderers that draw many passes a frame. Changing a buffer the GPU may still
// be reading makes the driver wait for the GPU, and mapping or orphaning a
// buffer for every pass costs a driver round trip each time. Here the buffer
// stays mapped (ARB_buffer_storage, persistent and coherent) and each pass's
// data is copied into the next free space with plain memcpy; a fence in each
// half of the ring makes sure the GPU is done with a half before it is reused.
// Without buffer storage it falls back to glBufferSubData into the ring.
class GLStreamRing
{
public:
    void Init(GLsizeiptr size)
    {
        // where the context is (the render thread, with GL on its own thread):
        // glBufferStorage is loaded here, outside glad
        GLThread::RunSync([&] { InitImpl(size); });
    }

    void InitImpl(GLsizeiptr size)
    {
        Size = size;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &Align);
        if (Align < 16) Align = 16;

        glGenBuffers(1, &Buffer);
        glBindBuffer(GL_UNIFORM_BUFFER, Buffer);
        using BufferStorageFunc = void (APIENTRY*)(GLenum, GLsizeiptr, const void*, GLbitfield);
        BufferStorageFunc bufferStorage = nullptr;
#ifdef _WIN32
        bufferStorage = (BufferStorageFunc)wglGetProcAddress("glBufferStorage");
#endif
        constexpr GLbitfield kPersistent = 0x0040, kCoherent = 0x0080;   // GL_MAP_PERSISTENT_BIT, GL_MAP_COHERENT_BIT
        if (bufferStorage)
        {
            bufferStorage(GL_UNIFORM_BUFFER, Size, nullptr, GL_MAP_WRITE_BIT | kPersistent | kCoherent);
            if (glGetError() == GL_NO_ERROR)
                Mapped = (u8*)glMapBufferRange(GL_UNIFORM_BUFFER, 0, Size, GL_MAP_WRITE_BIT | kPersistent | kCoherent);
        }
        if (!Mapped)
        {
            // no persistent mapping: a plain buffer, written with glBufferSubData
            glDeleteBuffers(1, &Buffer);
            glGenBuffers(1, &Buffer);
            glBindBuffer(GL_UNIFORM_BUFFER, Buffer);
            glBufferData(GL_UNIFORM_BUFFER, Size, nullptr, GL_STREAM_DRAW);
        }
        Pos = 0;
        Half = 0;
    }

    void Release()
    {
        GLThread::RunSync([&] { ReleaseImpl(); });
    }

    void ReleaseImpl()
    {
        for (GLsync& fence : Fences)
            if (fence) { glDeleteSync(fence); fence = nullptr; }
        if (Buffer)
        {
            if (Mapped)
            {
                glBindBuffer(GL_UNIFORM_BUFFER, Buffer);
                glUnmapBuffer(GL_UNIFORM_BUFFER);
            }
            glDeleteBuffers(1, &Buffer);
        }
        Buffer = 0;
        Mapped = nullptr;
    }

    // Writes bytes [from, from + length) of a block of blockSize bytes, and
    // returns the offset to bind the block at (blockSize bytes from there).
    // Only the written bytes of the bound block hold this data: the shader
    // must read nothing outside them.
    GLintptr Write(const void* block, GLsizeiptr blockSize, GLintptr from, GLsizeiptr length)
    {
        // the block starts so that its byte `from` lands at the free space
        GLintptr base = Pos - from;
        if (base < 0) base = 0;
        base = (base + Align - 1) / Align * Align;
        if (base + from < Pos) base += Align;
        // a block lies entirely in one half, so the half's fence covers its use
        if (Half == 0 && base + blockSize > Size / 2)
        {
            Enter(1);
            base = Size / 2;
        }
        else if (Half == 1 && base + blockSize > Size)
        {
            Enter(0);
            base = 0;
        }

        if (length > 0)
        {
            if (Mapped)
                memcpy(Mapped + base + from, (const u8*)block + from, length);
            else
            {
                glBindBuffer(GL_UNIFORM_BUFFER, Buffer);
                glBufferSubData(GL_UNIFORM_BUFFER, base + from, length, (const u8*)block + from);
            }
        }
        Pos = base + from + length;
        return base;
    }

    GLuint Buffer = 0;

private:
    // moving on to a half: the GPU must be done with what was drawn from it
    // last time round, which a fence placed when we left it marks
    void Enter(int half)
    {
        if (Fences[half])
        {
            glClientWaitSync(Fences[half], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
            glDeleteSync(Fences[half]);
            Fences[half] = nullptr;
        }
        // everything drawn so far used the half we are leaving
        if (Fences[1 - half]) glDeleteSync(Fences[1 - half]);
        Fences[1 - half] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        Half = half;
    }

    GLsizeiptr Size = 0;
    GLintptr Pos = 0;
    GLint Align = 256;
    int Half = 0;
    u8* Mapped = nullptr;
    GLsync Fences[2] {};
};

}
