// MultiMelon Wifi: the log (sdmc:/mmwifi.log), from any thread. It makes FS requests on the calling thread,
// so a server thread logs only after copying its request out of the command buffer.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

void LogF(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
