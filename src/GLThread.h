/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a fork of melonDS.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#pragma once

#include <functional>

namespace melonDS::GLThread
{

// OpenGL on its own thread, like Mesa's glthread. While it runs, a render
// thread holds the GL context and every GL call made through glad's function
// pointers is recorded into a queue instead, which the render thread replays
// in order: calls that only pass values return at once, data they point to is
// copied into the queue, and calls that return something (glGet*, glGen*,
// glReadPixels into memory, ...) wait for the render thread. The emulation
// thread then spends no time in the OpenGL driver, which runs alongside it.
//
// Only one thread at a time makes GL calls, as with a real context: the one
// that "made the context current". Hand it over with Release() (which waits
// for the queue to empty) on the old thread.

// Starts the render thread, which makes the context current with
// `makeCurrent` and releases it with `doneCurrent` when stopped. The context
// must not be current anywhere else.
void Start(std::function<void()> makeCurrent, std::function<void()> doneCurrent);

// Waits for everything queued, releases the context and ends the render thread.
// GL calls then go straight to the driver again (on a thread that makes the
// context current itself).
void Stop();

bool Running();

// Waits until the render thread has carried out everything queued so far.
void Sync();

// Hands GL over: waits for the queue to empty (call on the thread giving it up).
void Release();

// Queues a buffer swap. At most `maxPending` swaps wait in the queue; past
// that this waits, so the emulation thread stays at most that many frames
// ahead of what is on screen.
void Swap(std::function<void()> swap, int maxPending = 1);

// Runs `fn` on the thread that holds the context (the render thread while
// running, otherwise right here) and waits for it. For context-level calls
// such as the swap interval, and functions loaded outside glad.
void RunSync(const std::function<void()>& fn);

// Calls that had to wait for the render thread, for measuring.
unsigned long long SyncCount();

}
