#include "mini_test.h"

#include "Core/Dispatcher.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

using namespace aiie;

namespace {

void PumpMessagesUntil(std::function<bool()> pred, int timeoutMs = 2000) {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (pred()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeoutMs) {
            break;
        }
    }
}

} // namespace

TEST(immediate_dispatcher_executes_inline) {
    ImmediateDispatcher dispatcher;
    bool executed = false;
    dispatcher.Post([&] { executed = true; });
    CHECK(executed);
}

TEST(message_dispatcher_window_creation) {
    MessageDispatcher dispatcher;
    HWND hwnd = dispatcher.Window();
    CHECK(hwnd != nullptr);
    CHECK(IsWindow(hwnd) != FALSE);
}

TEST(message_dispatcher_post_single_task) {
    MessageDispatcher dispatcher;
    std::atomic<bool> executed{false};

    dispatcher.Post([&] { executed = true; });
    // Before message loop pumping, task should not have executed yet
    CHECK(!executed.load());

    PumpMessagesUntil([&] { return executed.load(); });
    CHECK(executed.load());
}

TEST(message_dispatcher_fifo_ordering) {
    MessageDispatcher dispatcher;
    std::vector<int> order;

    dispatcher.Post([&] { order.push_back(1); });
    dispatcher.Post([&] { order.push_back(2); });
    dispatcher.Post([&] { order.push_back(3); });

    PumpMessagesUntil([&] { return order.size() == 3; });

    CHECK_EQ(order.size(), 3u);
    if (order.size() == 3) {
        CHECK_EQ(order[0], 1);
        CHECK_EQ(order[1], 2);
        CHECK_EQ(order[2], 3);
    }
}

TEST(message_dispatcher_cross_thread_post) {
    MessageDispatcher dispatcher;
    const DWORD mainThreadId = GetCurrentThreadId();
    DWORD taskThreadId = 0;
    std::atomic<bool> executed{false};

    std::thread backgroundThread([&] {
        dispatcher.Post([&] {
            taskThreadId = GetCurrentThreadId();
            executed = true;
        });
    });
    backgroundThread.join();

    PumpMessagesUntil([&] { return executed.load(); });

    CHECK(executed.load());
    CHECK_EQ(taskThreadId, mainThreadId);
}

TEST(message_dispatcher_teardown) {
    HWND hwnd = nullptr;
    {
        MessageDispatcher dispatcher;
        hwnd = dispatcher.Window();
        CHECK(hwnd != nullptr);
        CHECK(IsWindow(hwnd) != FALSE);

        // Post a task that won't be pumped before destruction
        dispatcher.Post([] { });
    }
    // After destruction, the window should no longer exist
    CHECK(IsWindow(hwnd) == FALSE);
}
