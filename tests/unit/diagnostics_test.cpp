#include "mini_test.h"
#include "Core/Logging.h"
#include "Providers/MockProvider.h"
#include "UI/DiagnosticsWindow.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace aiie;

namespace {

HWND WaitForDiagnosticsWindow(int timeoutMs = 2000) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        HWND hwnd = FindWindowW(L"AIInputEngine.DiagnosticsWindow", nullptr);
        if (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) {
            return hwnd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return nullptr;
}

void PumpMessagesFor(int ms) {
    const auto start = std::chrono::steady_clock::now();
    MSG msg;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < ms) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

TEST(diagnostics_snapshot_formatting) {
    Logger::Instance().ClearRing();
    Logger::Instance().Info(cat::Core, L"Diagnostics snapshot test line 1");
    Logger::Instance().Warn(cat::Translation, L"Diagnostics snapshot test line 2");

    auto mock = std::make_shared<MockProvider>();
    const std::wstring snapshot = FormatDiagnosticsSnapshot(mock);

    CHECK(snapshot.find(L"--- AI Input Engine diagnostics ---") != std::wstring::npos);
    CHECK(snapshot.find(L"Provider: Mock (offline demo)") != std::wstring::npos);
    CHECK(snapshot.find(L"Diagnostics snapshot test line 1") != std::wstring::npos);
    CHECK(snapshot.find(L"Diagnostics snapshot test line 2") != std::wstring::npos);
    CHECK(snapshot.find(L"[INFO][CORE]") != std::wstring::npos);
    CHECK(snapshot.find(L"[WARN][TRANSLATION]") != std::wstring::npos);
}

TEST(diagnostics_window_lifecycle_and_controls) {
    CloseDiagnosticsWindow();
    CHECK(!IsDiagnosticsOpen());

    Logger::Instance().ClearRing();
    Logger::Instance().Info(cat::Ui, L"UI test entry for diagnostics window");

    auto mock = std::make_shared<MockProvider>();
    ShowDiagnosticsWindow(mock);

    HWND hwnd = WaitForDiagnosticsWindow();
    CHECK(hwnd != nullptr);
    CHECK(IsDiagnosticsOpen());

    // Verify child controls exist
    HWND edit = GetDlgItem(hwnd, 1);
    HWND btnCopy = GetDlgItem(hwnd, 101);
    HWND btnClear = GetDlgItem(hwnd, 102);
    HWND chkAuto = GetDlgItem(hwnd, 103);
    HWND lblStatus = GetDlgItem(hwnd, 104);

    CHECK(edit != nullptr);
    CHECK(btnCopy != nullptr);
    CHECK(btnClear != nullptr);
    CHECK(chkAuto != nullptr);
    CHECK(lblStatus != nullptr);

    wchar_t buf[2048] = {};
    GetWindowTextW(edit, buf, 2048);
    const std::wstring text(buf);
    CHECK(text.find(L"UI test entry for diagnostics window") != std::wstring::npos);

    // Verify single instance reuse
    ShowDiagnosticsWindow(mock);
    CHECK(IsDiagnosticsOpen());

    CloseDiagnosticsWindow();
    PumpMessagesFor(50);
    CHECK(!IsDiagnosticsOpen());
}

TEST(diagnostics_window_selection_preservation) {
    CloseDiagnosticsWindow();

    Logger::Instance().ClearRing();
    for (int i = 0; i < 20; ++i) {
        Logger::Instance().Info(cat::Core, std::format(L"Log entry line {}", i));
    }

    auto mock = std::make_shared<MockProvider>();
    ShowDiagnosticsWindow(mock);
    HWND hwnd = WaitForDiagnosticsWindow();
    CHECK(hwnd != nullptr);

    HWND edit = GetDlgItem(hwnd, 1);
    CHECK(edit != nullptr);

    // Set selection in the edit control
    SendMessageW(edit, EM_SETSEL, 20, 50);

    DWORD selStart = 0, selEnd = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
    CHECK_EQ(selStart, 20);
    CHECK_EQ(selEnd, 50);

    // Add a new log entry and trigger timer refresh
    Logger::Instance().Info(cat::Core, L"Newly arrived log line while selected");
    SendMessageW(hwnd, WM_TIMER, 1, 0); // kRefreshTimer

    // Verify selection is STILL preserved and NOT reset to 0
    DWORD newSelStart = 0, newSelEnd = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&newSelStart), reinterpret_cast<LPARAM>(&newSelEnd));
    CHECK_EQ(newSelStart, 20);
    CHECK_EQ(newSelEnd, 50);

    CloseDiagnosticsWindow();
    PumpMessagesFor(50);
}

TEST(diagnostics_window_copy_all_and_clear) {
    CloseDiagnosticsWindow();

    Logger::Instance().ClearRing();
    Logger::Instance().Info(cat::Core, L"CopyAll specific marker 12345");

    auto mock = std::make_shared<MockProvider>();
    ShowDiagnosticsWindow(mock);
    HWND hwnd = WaitForDiagnosticsWindow();
    CHECK(hwnd != nullptr);

    // Test Copy All button click
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(101, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(hwnd, 101)));
    PumpMessagesFor(50);

    // Check status label updated
    HWND lblStatus = GetDlgItem(hwnd, 104);
    wchar_t statusBuf[128] = {};
    GetWindowTextW(lblStatus, statusBuf, 128);
    CHECK_STREQ(statusBuf, L"Diagnostics copied to clipboard!");

    // Verify clipboard contains the marker text
    if (OpenClipboard(hwnd)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const wchar_t* pText = static_cast<const wchar_t*>(GlobalLock(hData));
            if (pText) {
                const std::wstring clipText(pText);
                GlobalUnlock(hData);
                CHECK(clipText.find(L"CopyAll specific marker 12345") != std::wstring::npos);
            }
        }
        CloseClipboard();
    }

    // Test Clear button click
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(102, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(hwnd, 102)));
    PumpMessagesFor(50);

    HWND edit = GetDlgItem(hwnd, 1);
    wchar_t editBuf[2048] = {};
    GetWindowTextW(edit, editBuf, 2048);
    const std::wstring clearedText(editBuf);
    // After clear, the ring logs should be gone
    CHECK(clearedText.find(L"CopyAll specific marker 12345") == std::wstring::npos);

    CloseDiagnosticsWindow();
    PumpMessagesFor(50);
}

TEST(diagnostics_logging_tickms_monotonic) {
    Logger::Instance().ClearRing();
    Logger::Instance().Info(cat::Core, L"TickMs test entry 1");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Logger::Instance().Info(cat::Core, L"TickMs test entry 2");

    const auto snapshot = Logger::Instance().Snapshot();
    CHECK(snapshot.size() >= 2u);
    if (snapshot.size() >= 2) {
        CHECK(snapshot[0].tickMs > 0);
        CHECK(snapshot[1].tickMs > 0);
        CHECK(snapshot[1].tickMs >= snapshot[0].tickMs);
    }
}
