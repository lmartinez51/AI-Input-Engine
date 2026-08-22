#pragma once

#include <windows.h>

#include <functional>
#include <memory>

namespace aiie {

// Abstraction over "run this function on the input thread".
// Implementations: message-window-based dispatcher (hook/TSF backends),
// manual dispatcher in tests, immediate dispatcher where thread-safety is
// guaranteed by the caller.
class IDispatcher {
public:
    virtual ~IDispatcher() = default;
    virtual void Post(std::function<void()> task) = 0;
};

// Message-window-based dispatcher. The window is created on the owning thread,
// so posting is safe from any thread (PostMessage) and tasks execute on the
// window's thread. The owning thread must run a message loop.
class MessageDispatcher : public IDispatcher {
public:
    MessageDispatcher();
    ~MessageDispatcher() override;
    MessageDispatcher(const MessageDispatcher&) = delete;
    MessageDispatcher& operator=(const MessageDispatcher&) = delete;

    void Post(std::function<void()> task) override;
    HWND Window() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    HWND hwnd_ = nullptr;
};

// Runs tasks immediately on the calling thread. For use where the caller is
// already the input thread (e.g., TSF service where the pipeline callback is
// marshalled explicitly).
class ImmediateDispatcher : public IDispatcher {
public:
    void Post(std::function<void()> task) override;
};

} // namespace aiie