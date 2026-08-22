#include "mini_test.h"

#include <windows.h>

#include <cstdio>

#include <psapi.h>

static LONG WINAPI CrashFilter(_EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "CRASH: code=0x%08lX at 0x%p\n",
                 ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    HMODULE mods[128];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count && i < 128; ++i) {
            const auto base = reinterpret_cast<std::uintptr_t>(mods[i]);
            const auto addr = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
            if (addr >= base && addr < base + 0x10000000) {
                char name[MAX_PATH] = {};
                GetModuleFileNameA(mods[i], name, MAX_PATH);
                std::fprintf(stderr, "  in %s +0x%llX\n", name, addr - base);
                break;
            }
        }
    }
    std::fprintf(stderr, "  thread: 0x%08lX  context flags 0x%lX\n", GetCurrentThreadId(),
                 ep->ContextRecord->ContextFlags);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain() {
    SetUnhandledExceptionFilter(&CrashFilter);
    // The unit tests exercise console output; ensure UTF-8 output works for
    // any wide strings printed by failure messages.
    SetConsoleOutputCP(CP_UTF8);
    return ::mt::RunAll();
}