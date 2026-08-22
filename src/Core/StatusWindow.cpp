#include "Core/StatusWindow.h"

namespace aiie {

namespace {
constexpr wchar_t kClass[] = L"AIInputEngine.StatusIndicator";
constexpr UINT kHideTimer = 1;
constexpr UINT kAutoHideMs = 4000;
} // namespace

StatusWindow::StatusWindow() {
    static bool registered = [] {
        WNDCLASS wc{};
        wc.lpfnWndProc = &StatusWindow::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassW(&wc) != 0;
    }();
    (void)registered;
    hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
}

StatusWindow::~StatusWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void StatusWindow::Show(std::wstring_view text) {
    std::lock_guard lock(mutex_);
    SetWindowTextW(hwnd_, std::wstring(text).c_str());
    visible_ = true;
    SetTimer(hwnd_, kHideTimer, kAutoHideMs, nullptr);
    // Size to content and position top-right of the primary screen.
    HDC hdc = GetDC(hwnd_);
    HGDIOBJ oldFont = SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
    RECT rc{0, 0, 0, 0};
    DrawTextW(hdc, std::wstring(text).c_str(), -1, &rc, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    ReleaseDC(hwnd_, hdc);
    const int w = rc.right + 28;
    const int h = 26;
    const int x = GetSystemMetrics(SM_CXSCREEN) - w - 24;
    const int y = 24;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void StatusWindow::Hide() {
    std::lock_guard lock(mutex_);
    if (visible_) {
        KillTimer(hwnd_, kHideTimer);
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

bool StatusWindow::Visible() const {
    std::lock_guard lock(mutex_);
    return visible_;
}

LRESULT CALLBACK StatusWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCREATE: {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_TIMER:
        if (wp == kHideTimer) {
            ShowWindow(hwnd, SW_HIDE);
            KillTimer(hwnd, kHideTimer);
            if (auto* self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                std::lock_guard lock(self->mutex_);
                self->visible_ = false;
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(245, 245, 245));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        FrameRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        const int len = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(hwnd, text.data(), len + 1);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(15, 15, 15));
        HGDIOBJ oldFont = SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
        rc.left += 8;
        rc.right -= 8;
        DrawTextW(hdc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace aiie