#include "Core/Dispatcher.h"

#include <windows.h>

#include <utility>

namespace aiie {

namespace {
constexpr UINT kMsgTask = WM_APP + 1;
}

MessageDispatcher::MessageDispatcher() {
    // Created with a plain, non-visible window class. The window lives on the
    // thread that constructs the dispatcher.
    static const wchar_t kClass[] = L"AIInputEngine.MessageDispatcher";
    static bool registered = [] {
        WNDCLASS wc{};
        wc.lpfnWndProc = &MessageDispatcher::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        return RegisterClassW(&wc) != 0;
    }();
    (void)registered;
    hwnd_ = CreateWindowExW(0, kClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            GetModuleHandleW(nullptr), this);
}

MessageDispatcher::~MessageDispatcher() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

void MessageDispatcher::Post(std::function<void()> task) {
    auto* boxed = new std::function<void()>(std::move(task));
    if (!PostMessageW(hwnd_, kMsgTask, 0, reinterpret_cast<LPARAM>(boxed))) {
        delete boxed;
    }
}

LRESULT CALLBACK MessageDispatcher::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgTask) {
        auto* task = reinterpret_cast<std::function<void()>*>(lp);
        (*task)();
        delete task;
        return 0;
    }
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ImmediateDispatcher::Post(std::function<void()> task) {
    task();
}

} // namespace aiie