/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a melonDS fork.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

// widemelon-room-server [port]: the room server for WideMelon's "Online as Local", on
// its own (default UDP port 7300). Both players' WideMelons point at this PC's address.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "OnlineRelay.h"

int main(int argc, char** argv)
{
    const int port = argc > 1 ? std::atoi(argv[1]) : OnlineProto::DefaultPort;
    OnlineRelay relay;
    const bool ok = relay.RunHere((uint16_t)port, [](const std::string& s) {
        const std::time_t t = std::time(nullptr);
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::printf("[%s] %s\n", when, s.c_str());
        std::fflush(stdout);
    });
    return ok ? 0 : 1;
}
