#include "Translation/Debouncer.h"

namespace aiie {

Debouncer::Debouncer(FiredCallback onFired) : onFired_(std::move(onFired)) {}

void Debouncer::SetDelay(std::chrono::milliseconds delay) {
    std::lock_guard lock(mutex_);
    delay_ = delay;
}

void Debouncer::Arm() {
    std::lock_guard lock(mutex_);
    armed_ = true;
    armedAt_ = Now();
}

bool Debouncer::FireNow() {
    {
        std::lock_guard lock(mutex_);
        if (!armed_) {
            return false;
        }
        armed_ = false;
    }
    if (onFired_) {
        onFired_();
    }
    return true;
}

void Debouncer::Disarm() {
    std::lock_guard lock(mutex_);
    armed_ = false;
}

bool Debouncer::Tick() {
    bool fire = false;
    {
        std::lock_guard lock(mutex_);
        if (armed_ && Now() - armedAt_ >= delay_) {
            armed_ = false;
            fire = true;
        }
    }
    if (fire && onFired_) {
        onFired_();
    }
    return fire;
}

void Debouncer::Start() {
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
    }
    thread_ = std::thread([this] {
        for (;;) {
            if (Tick()) {
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!running_.load()) {
                break;
            }
        }
    });
}

void Debouncer::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::chrono::steady_clock::time_point Debouncer::Now() const {
    if (nowOverride_.load()) {
        return fakeNow_.load();
    }
    return Clock::now();
}

} // namespace aiie