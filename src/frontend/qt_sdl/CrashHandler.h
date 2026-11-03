// MultiMelon: a crash report when the program crashes.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace CrashHandler
{

// From then on, a crash (an access violation, an abort, an unhandled C++ exception...) writes
// crashes/crash-<date>.txt (the error, where it happened, and the call stack as offsets in the
// exe and its DLLs, which an unstripped copy of the same build turns into function names with
// tools/crash_symbols.py) and crash-<date>.dmp (a minidump, for a debugger) next to the exe.
// Windows only; elsewhere it does nothing.
void Install();

}
