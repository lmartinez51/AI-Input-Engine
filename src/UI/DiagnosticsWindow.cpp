#include "UI/DiagnosticsWindow.h"

#include "Core/Logging.h"

#include <atomic>
#include <string>
#include <vector>

namespace aiie {

namespace {

constexpr wchar_t kClass[] = L"AIInputEngine.DiagnosticsWindow";
constexpr UINT kRefreshTimer = 1;
constexpr UINT kRefreshMs = 500;

std::atomic<HWND> g_hwnd{nullptr};
std::shared_ptr<ITranslationProvider> g_provider;

const wchar_t* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Error:
        return L"ERR ";
    case LogLevel::Warn:
        return L"WARN";
    case LogLevel::Info:
        return L"INFO";
    case LogLevel::Debug:
        return L"DBG ";
    case LogLevel::Trace:
        return L"TRACE";
    }
    return L"?";
}

std::wstring BuildSnapshotText() {
    const auto entries = Logger::Instance().Snapshot();
    std::wstring text;
    text.reserve(entries.size() * 128);
    for (const auto& e : entries) {
        const auto seconds = e.tickMs / 1000;
        text += std::to_wstring(seconds);
        text += L'.';
        text += std::to_wstring(e.tickMs % 1000);
        text += L" [";
        text += LevelName(e.level);
        text += L"][";
        text += e.category;
        text += L"] ";
        text += e.message;
        text += L"\r\n";
    }

    const ProviderStatus status = g_provider ? g_provider->Status() : ProviderStatus{};
    const wchar_t* state = L"unknown";
    switch (status.state) {
    case ProviderStatus::State::Ready:
        state = L"ready";
        break;
    case ProviderStatus::State::Busy:
        state = L"busy";
        break;
    case ProviderStatus::State::Error:
        state = L"error";
        break;
    case ProviderStatus::State::Unavailable:
        state = L"unavailable";
        break;
    case ProviderStatus::State::Unknown:
        break;
    }
    std::wstring header = L"--- AI Input Engine diagnostics ---\r\nProvider: ";
    header += g_provider ? g_provider->DisplayName() : L"(none)";
    header += L" | status: ";
    header += state;
    if (!status.message.empty()) {
        header += L" | ";
        header += status.message;
    }
    header += L"\r\n\r\n";
    return header + text;
}

struct Local {
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_NCCREATE:
            return TRUE;
        case WM_CREATE:
            SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kRefreshTimer) {
                HWND edit = GetDlgItem(hwnd, 1);
                if (edit) {
                    SetWindowTextW(edit, BuildSnapshotText().c_str());
                }
            }
            return 0;
        case WM_SIZE: {
            HWND edit = GetDlgItem(hwnd, 1);
            if (edit) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                SetWindowPos(edit, nullptr, 0, 0, rc.right, rc.bottom,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kRefreshTimer);
            g_hwnd.store(nullptr);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
};

} // namespace

void ShowDiagnosticsWindow(std::shared_ptr<ITranslationProvider> provider, HWND parent) {
    HWND existing = g_hwnd.load();
    if (existing && IsWindow(existing)) {
        ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
        return;
    }

    static bool registered = [] {
        WNDCLASS wc{};
        wc.lpfnWndProc = &Local::Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        return RegisterClassW(&wc) != 0;
    }();

    g_provider = std::move(provider);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"AI Input Engine \u2014 Diagnostics",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                760, 480, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        return;
    }

    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL | WS_TABSTOP,
                                0, 0, 760, 480, hwnd, reinterpret_cast<HMENU>(1),
                                GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
    SetWindowTextW(edit, BuildSnapshotText().c_str());

    g_hwnd.store(hwnd);
    (void)registered;
}

bool IsDiagnosticsOpen() {
    HWND hwnd = g_hwnd.load();
    return hwnd != nullptr && IsWindow(hwnd);
}

} // namespace aiie