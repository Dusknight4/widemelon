// MultiMelon: a crash report when the program crashes (see CrashHandler.h).
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CrashHandler.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "version.h"

namespace CrashHandler
{

namespace
{

wchar_t g_dir[MAX_PATH];        // <exe folder>\crashes
volatile LONG g_handling = 0;   // (one report, even if the report itself crashes)

// "module+0xoffset" for an address
void Where(DWORD64 addr, char* out, size_t size)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod)
    {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        snprintf(out, size, "%ls+0x%llx", name, (unsigned long long)(addr - (DWORD64)mod));
    }
    else
        snprintf(out, size, "0x%llx", (unsigned long long)addr);
}

void Report(EXCEPTION_POINTERS* ep, const char* what)
{
    if (InterlockedExchange(&g_handling, 1)) return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04d-%02d-%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CreateDirectoryW(g_dir, nullptr);
    wchar_t txtPath[MAX_PATH], dmpPath[MAX_PATH];
    swprintf(txtPath, MAX_PATH, L"%ls\\crash-%ls.txt", g_dir, stamp);
    swprintf(dmpPath, MAX_PATH, L"%ls\\crash-%ls.dmp", g_dir, stamp);

    FILE* f = _wfopen(txtPath, L"w");
    if (f)
    {
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fprintf(f, "MultiMelon %s crashed: %s\nexe: %ls\nthread %lu\n", WIDEMELON_VERSION, what, exe, GetCurrentThreadId());

        CONTEXT ctx;
        if (ep && ep->ExceptionRecord)
        {
            const EXCEPTION_RECORD* r = ep->ExceptionRecord;
            char where[512];
            Where((DWORD64)r->ExceptionAddress, where, sizeof(where));
            fprintf(f, "exception 0x%08lx at %s", r->ExceptionCode, where);
            if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
                fprintf(f, " (%s 0x%llx)", r->ExceptionInformation[0] == 0 ? "reading" : r->ExceptionInformation[0] == 1 ? "writing" : "executing",
                        (unsigned long long)r->ExceptionInformation[1]);
            fprintf(f, "\n");
            ctx = *ep->ContextRecord;
        }
        else
        {
            RtlCaptureContext(&ctx);
        }
        fprintf(f, "rip %016llx rsp %016llx rbp %016llx\nrax %016llx rbx %016llx rcx %016llx rdx %016llx\n"
                   "rsi %016llx rdi %016llx r8 %016llx r9 %016llx\n",
                (unsigned long long)ctx.Rip, (unsigned long long)ctx.Rsp, (unsigned long long)ctx.Rbp,
                (unsigned long long)ctx.Rax, (unsigned long long)ctx.Rbx, (unsigned long long)ctx.Rcx,
                (unsigned long long)ctx.Rdx, (unsigned long long)ctx.Rsi, (unsigned long long)ctx.Rdi,
                (unsigned long long)ctx.R8, (unsigned long long)ctx.R9);

        // the call stack (x64 unwind data, which MinGW's code has too)
        fprintf(f, "\nstack:\n");
        HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
        SymInitialize(proc, nullptr, TRUE);
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = ctx.Rip;    frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        for (int i = 0; i < 64; i++)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
                break;
            char where[512];
            Where(frame.AddrPC.Offset, where, sizeof(where));
            fprintf(f, "  %2d %s\n", i, where);
        }
        fclose(f);
    }

    HANDLE dmp = CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dmp != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION info = {GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dmp,
                          (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                          ep ? &info : nullptr, nullptr, nullptr);
        CloseHandle(dmp);
    }
}

LONG WINAPI OnException(EXCEPTION_POINTERS* ep)
{
    Report(ep, "an exception");
    return EXCEPTION_CONTINUE_SEARCH;     // (then Windows ends the program, as before)
}

void OnAbort(int)
{
    Report(nullptr, "abort()");
    std::_Exit(3);
}

void OnTerminate()
{
    Report(nullptr, "an unhandled C++ exception (std::terminate)");
    std::_Exit(3);
}

}

void Install()
{
    GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;
    wcsncat(g_dir, L"\\crashes", MAX_PATH - wcslen(g_dir) - 1);

    SetUnhandledExceptionFilter(OnException);
    std::signal(SIGABRT, OnAbort);
    std::set_terminate(OnTerminate);
}

}

#else

namespace CrashHandler
{
void Install() {}
}

#endif
