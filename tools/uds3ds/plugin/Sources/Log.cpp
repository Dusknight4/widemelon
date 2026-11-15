// MultiMelon Wifi: the log (see Log.h)
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Log.h"

#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
LightLock g_logLock;
bool g_logReady = false;
}

void LogF(const char* fmt, ...)
{
    if (!g_logReady)    // (first called from PatchProcess, before any other thread)
    {
        LightLock_Init(&g_logLock);
        g_logReady = true;
    }
    char line[256];
    const u64 t = osGetTime();
    int n = std::snprintf(line, sizeof(line), "[%llu.%03llu] ", (unsigned long long)(t / 1000) % 100000,
                          (unsigned long long)(t % 1000));
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line + n, sizeof(line) - n, fmt, a);
    va_end(a);
    LightLock_Lock(&g_logLock);
    CTRPluginFramework::File f("/mmwifi.log", CTRPluginFramework::File::WRITE | CTRPluginFramework::File::CREATE |
                                                  CTRPluginFramework::File::APPEND);
    if (f.IsOpen()) f.Write(line, std::strlen(line));
    LightLock_Unlock(&g_logLock);
}
