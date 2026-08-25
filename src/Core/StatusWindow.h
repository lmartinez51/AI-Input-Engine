#pragma once

#include <windows.h>

#include <mutex>
#include <string>

namespace aiie {

// Small borderless "Translating..." indicator. Never activates and never
// steals keyboard focus (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST).
// Show()/Hide() are callable from any thread; the window is created on the
// thread that constructs the object, which must run a message loop.
class StatusWindow {
public:
    StatusWindow();
    virtual ~StatusWindow();

    StatusWindow(const StatusWindow&) = delete;
    StatusWindow& operator=(const StatusWindow&) = delete;

    virtual void Show(std::wstring_view text); // auto-hides after a few seconds
    virtual void Hide();
    bool Visible() const;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void UpdateWindowText();

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    mutable std::mutex mutex_;
};

} // namespace aiie