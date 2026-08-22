/**
 * @file Main.cpp
 * @brief Application entry point and single-instance mutex initialization.
 * @details AIInputEngine is a native Windows background utility that intercepts
 *          and enhances global text input using Large Language Models (LLMs)
 *          such as Gemini and OpenAI. It runs silently in the system tray,
 *          providing hotkey-triggered AI text processing, smart caching, and
 *          seamless text replacement across any active Windows application.
 * @author Lorenzo Martínez
 * @date 2026-08-22
 * @version 1.0.0
 */

#include "UI/TrayApp.h"

#include "Core/Logging.h"

#include <windows.h>

namespace {

/**
 * @brief Named mutex identifier used to enforce a single running application instance.
 */
constexpr wchar_t kMutexName[] = L"Local\\AIInputEngine.SingleInstance";

} // namespace

/**
 * @brief Windows application entry point.
 * @param instance Handle to the current instance of the application.
 * @param HINSTANCE Unused legacy instance handle parameter.
 * @param PWSTR Unused command line string parameter.
 * @param int Unused show command flag parameter.
 * @return int Status code on application exit (0 on success or duplicate instance, 1 on failure).
 * @note Creates a named mutex to ensure only one instance of the tray application runs concurrently.
 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Single instance: a second launch simply exits.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    aiie::TrayApp app;
    if (!app.Init(instance)) {
        CloseHandle(mutex);
        return 1;
    }
    const int result = app.Run();

    CloseHandle(mutex);
    return result;
}