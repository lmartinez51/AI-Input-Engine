#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace aiie {

// Debounce logic extracted for testability. "Fires" when the configured idle
// interval elapses after the last Arm() call. Thread-safe.
class Debouncer {
public:
    using Clock = std::chrono::steady_clock;
    using FiredCallback = std::function<void()>;

    explicit Debouncer(FiredCallback onFired);

    void SetDelay(std::chrono::milliseconds delay);

    // Arm/restart the idle window.
    void Arm();
    // Force the pending timer to fire immediately (sentence terminators).
    // Returns false if nothing was pending.
    bool FireNow();
    void Disarm();

    // Poll: fires the callback (on the calling thread) when the idle window
    // elapsed. Returns true if the callback was invoked.
    bool Tick();

    // Dedicated-thread mode: the internal thread polls and invokes onFired.
    void Start();
    void Stop();

    bool Armed() const { return armed_; }

    // Test hook: inject a fake clock.
    void SetNowForTest(std::chrono::steady_clock::time_point now) {
        nowOverride_.store(true);
        fakeNow_.store(now);
    }
    void ClearNowForTest() { nowOverride_.store(false); }

private:
    std::chrono::steady_clock::time_point Now() const;
    void FireLocked();

    std::mutex mutex_;
    std::condition_variable cv_;
    FiredCallback onFired_;
    std::chrono::milliseconds delay_{500};
    bool armed_ = false;
    std::chrono::steady_clock::time_point armedAt_{};

    std::atomic<bool> nowOverride_{false};
    std::atomic<std::chrono::steady_clock::time_point> fakeNow_{};

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace aiie