#include "mini_test.h"

#include <windows.h>

#include <cstdio>

#include <psapi.h>
#include <commctrl.h>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

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

    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    void* stack[64];
    const USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);
    for (USHORT i = 0; i < frames; ++i) {
        DWORD64 displacement = 0;
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;

        if (SymFromAddr(GetCurrentProcess(), (DWORD64)stack[i], &displacement, pSymbol)) {
            IMAGEHLP_LINE64 line;
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)stack[i], &lineDisplacement, &line)) {
                std::fprintf(stderr, "  #%d %s [%s:%lu]\n", i, pSymbol->Name, line.FileName, line.LineNumber);
            } else {
                std::fprintf(stderr, "  #%d %s +0x%llX\n", i, pSymbol->Name, displacement);
            }
        } else {
            std::fprintf(stderr, "  #%d 0x%p\n", i, stack[i]);
        }
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain() {
    SetUnhandledExceptionFilter(&CrashFilter);
    // The unit tests exercise console output; ensure UTF-8 output works for
    // any wide strings printed by failure messages.
    SetConsoleOutputCP(CP_UTF8);
    INITCOMMONCONTROLSEX icex{sizeof(icex), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_USEREX_CLASSES};
    InitCommonControlsEx(&icex);
    return ::mt::RunAll();
}