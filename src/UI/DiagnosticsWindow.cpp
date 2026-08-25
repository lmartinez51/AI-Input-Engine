#include "UI/DiagnosticsWindow.h"

#include "Core/Logging.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace aiie {

namespace {

constexpr wchar_t kClass[] = L"AIInputEngine.DiagnosticsWindow";
constexpr UINT kRefreshTimer = 1;
constexpr UINT kStatusTimer = 2;
constexpr UINT kRefreshMs = 500;
constexpr UINT kStatusMs = 2500;

constexpr int kEdit = 1;
constexpr int kCopyAll = 101;
constexpr int kClear = 102;
constexpr int kAutoScroll = 103;
constexpr int kStatusLabel = 104;

constexpr int kToolbarHeight = 38;

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

struct WindowState {
    std::wstring lastText;
    bool autoScroll = true;
    HFONT hFont = nullptr;
    HFONT hBtnFont = nullptr;
    WNDPROC origEditProc = nullptr;
};

LRESULT CALLBACK EditWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) < 0);
        if (ctrl && (wp == 'A' || wp == 'a')) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        if (ctrl && (wp == 'C' || wp == 'c')) {
            SendMessageW(hwnd, WM_COPY, 0, 0);
            return 0;
        }
        break;
    }
    }
    HWND parent = GetParent(hwnd);
    if (parent) {
        auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(parent, GWLP_USERDATA));
        if (state && state->origEditProc) {
            return CallWindowProcW(state->origEditProc, hwnd, msg, wp, lp);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void UpdateDiagnosticsContent(HWND hwnd, WindowState* state, bool force) {
    if (!state) return;
    HWND edit = GetDlgItem(hwnd, kEdit);
    if (!edit) return;

    const std::wstring newText = FormatDiagnosticsSnapshot(g_provider);
    if (!force && newText == state->lastText) {
        return; // Content unchanged: do not touch control state
    }

    // 1. Capture current user selection
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
    const bool hasSelection = (selStart != selEnd);

    // 2. Capture vertical scroll info & check if viewport was at the tail
    SCROLLINFO si{sizeof(si), SIF_ALL};
    GetScrollInfo(edit, SB_VERT, &si);
    const bool wasAtBottom = (si.nPage > 0) && (si.nPos + static_cast<int>(si.nPage) >= si.nMax - 2);
    const int firstVisibleLine = static_cast<int>(SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));

    // 3. Freeze redrawing while updating buffer
    SendMessageW(edit, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(edit, newText.c_str());
    state->lastText = newText;

    // 4. Restore selection if user had highlighted text
    if (hasSelection) {
        const DWORD maxLen = static_cast<DWORD>(newText.size());
        selStart = std::min(selStart, maxLen);
        selEnd = std::min(selEnd, maxLen);
        SendMessageW(edit, EM_SETSEL, selStart, selEnd);
    }

    // 5. Follow tail only if user was already at bottom AND not actively selecting
    if (wasAtBottom && state->autoScroll && !hasSelection) {
        SendMessageW(edit, EM_LINESCROLL, 0, 0x7FFFFFFF);
    } else {
        const int currentFirstLine = static_cast<int>(SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
        const int lineDiff = firstVisibleLine - currentFirstLine;
        if (lineDiff != 0) {
            SendMessageW(edit, EM_LINESCROLL, 0, lineDiff);
        }
    }

    // 6. Unfreeze and repaint
    SendMessageW(edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(edit, nullptr, TRUE);
}

void DoCopyAll(HWND hwnd, WindowState* state) {
    std::wstring text;
    if (state && !state->lastText.empty()) {
        text = state->lastText;
    } else {
        HWND edit = GetDlgItem(hwnd, kEdit);
        if (edit) {
            const int len = GetWindowTextLengthW(edit);
            if (len > 0) {
                text.resize(len + 1);
                GetWindowTextW(edit, text.data(), len + 1);
                text.resize(len);
            }
        }
    }

    if (text.empty()) {
        text = FormatDiagnosticsSnapshot(g_provider);
    }

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hGlob) {
            void* ptr = GlobalLock(hGlob);
            if (ptr) {
                memcpy(ptr, text.c_str(), bytes);
                GlobalUnlock(hGlob);
                SetClipboardData(CF_UNICODETEXT, hGlob);
            }
        }
        CloseClipboard();

        HWND lbl = GetDlgItem(hwnd, kStatusLabel);
        if (lbl) {
            SetWindowTextW(lbl, L"Diagnostics copied to clipboard!");
            SetTimer(hwnd, kStatusTimer, kStatusMs, nullptr);
        }
    }
}

void DoClear(HWND hwnd, WindowState* state) {
    Logger::Instance().ClearRing();
    UpdateDiagnosticsContent(hwnd, state, true);
    HWND lbl = GetDlgItem(hwnd, kStatusLabel);
    if (lbl) {
        SetWindowTextW(lbl, L"Log buffer cleared.");
        SetTimer(hwnd, kStatusTimer, kStatusMs, nullptr);
    }
}

struct Local {
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE: {
            auto* state = new WindowState();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            state->hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            if (!state->hFont) {
                state->hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            }

            state->hBtnFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            if (!state->hBtnFont) {
                state->hBtnFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            }

            HWND btnCopy = CreateWindowExW(0, L"BUTTON", L"Copy All",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                           10, 6, 85, 26, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyAll)),
                                           GetModuleHandleW(nullptr), nullptr);
            SendMessageW(btnCopy, WM_SETFONT, reinterpret_cast<WPARAM>(state->hBtnFont), TRUE);

            HWND btnClear = CreateWindowExW(0, L"BUTTON", L"Clear",
                                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                            105, 6, 65, 26, hwnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kClear)),
                                            GetModuleHandleW(nullptr), nullptr);
            SendMessageW(btnClear, WM_SETFONT, reinterpret_cast<WPARAM>(state->hBtnFont), TRUE);

            HWND chkAuto = CreateWindowExW(0, L"BUTTON", L"Auto-scroll",
                                           WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                                           180, 8, 95, 22, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAutoScroll)),
                                           GetModuleHandleW(nullptr), nullptr);
            SendMessageW(chkAuto, WM_SETFONT, reinterpret_cast<WPARAM>(state->hBtnFont), TRUE);
            Button_SetCheck(chkAuto, BST_CHECKED);

            HWND lblStatus = CreateWindowExW(0, L"STATIC", L"",
                                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                                             285, 10, 320, 20, hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabel)),
                                             GetModuleHandleW(nullptr), nullptr);
            SendMessageW(lblStatus, WM_SETFONT, reinterpret_cast<WPARAM>(state->hBtnFont), TRUE);

            HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_TABSTOP,
                                        0, kToolbarHeight, 760, 480 - kToolbarHeight, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEdit)),
                                        GetModuleHandleW(nullptr), nullptr);
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->hFont), TRUE);
            state->origEditProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditWndProc)));

            UpdateDiagnosticsContent(hwnd, state, true);
            SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
            return 0;
        }
        case WM_COMMAND: {
            auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            switch (LOWORD(wp)) {
            case kCopyAll:
                DoCopyAll(hwnd, state);
                return 0;
            case kClear:
                DoClear(hwnd, state);
                return 0;
            case kAutoScroll: {
                if (state) {
                    HWND chk = GetDlgItem(hwnd, kAutoScroll);
                    state->autoScroll = (Button_GetCheck(chk) == BST_CHECKED);
                    if (state->autoScroll) {
                        HWND edit = GetDlgItem(hwnd, kEdit);
                        if (edit) {
                            SendMessageW(edit, EM_LINESCROLL, 0, 0x7FFFFFFF);
                        }
                    }
                }
                return 0;
            }
            }
            break;
        }
        case WM_TIMER: {
            auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (wp == kRefreshTimer) {
                UpdateDiagnosticsContent(hwnd, state, false);
            } else if (wp == kStatusTimer) {
                KillTimer(hwnd, kStatusTimer);
                HWND lbl = GetDlgItem(hwnd, kStatusLabel);
                if (lbl) {
                    SetWindowTextW(lbl, L"");
                }
            }
            return 0;
        }
        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HWND edit = GetDlgItem(hwnd, kEdit);
            if (edit) {
                SetWindowPos(edit, nullptr, 0, kToolbarHeight, rc.right,
                             std::max(0, static_cast<int>(rc.bottom - kToolbarHeight)),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kRefreshTimer);
            KillTimer(hwnd, kStatusTimer);
            return 0;
        case WM_NCDESTROY: {
            auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (state) {
                if (state->hFont && state->hFont != GetStockObject(DEFAULT_GUI_FONT)) {
                    DeleteObject(state->hFont);
                }
                if (state->hBtnFont && state->hBtnFont != GetStockObject(DEFAULT_GUI_FONT)) {
                    DeleteObject(state->hBtnFont);
                }
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            g_hwnd.store(nullptr);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

} // namespace

std::wstring FormatDiagnosticsSnapshot(std::shared_ptr<ITranslationProvider> provider) {
    const auto entries = Logger::Instance().Snapshot();
    std::wstring text;
    text.reserve(entries.size() * 128);
    for (const auto& e : entries) {
        const auto seconds = e.tickMs / 1000;
        text += std::to_wstring(seconds);
        text += L'.';
        if (e.tickMs % 1000 < 100) text += L'0';
        if (e.tickMs % 1000 < 10) text += L'0';
        text += std::to_wstring(e.tickMs % 1000);
        text += L" [";
        text += LevelName(e.level);
        text += L"][";
        text += e.category;
        text += L"] ";
        text += e.message;
        text += L"\r\n";
    }

    const ProviderStatus status = provider ? provider->Status() : ProviderStatus{};
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
    header += provider ? provider->DisplayName() : L"(none)";
    header += L" | status: ";
    header += state;
    if (!status.message.empty()) {
        header += L" | ";
        header += status.message;
    }
    header += L"\r\n\r\n";
    return header + text;
}

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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        return RegisterClassW(&wc) != 0;
    }();

    g_provider = std::move(provider);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"AI Input Engine \u2014 Diagnostics",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                780, 520, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        return;
    }

    g_hwnd.store(hwnd);
    (void)registered;
}

bool IsDiagnosticsOpen() {
    HWND hwnd = g_hwnd.load();
    return hwnd != nullptr && IsWindow(hwnd);
}

void CloseDiagnosticsWindow() {
    HWND existing = g_hwnd.load();
    if (existing && IsWindow(existing)) {
        DestroyWindow(existing);
    }
    g_hwnd.store(nullptr);
}

} // namespace aiie