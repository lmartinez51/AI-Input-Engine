#include "Translation/Debouncer.h"

namespace aiie {

Debouncer::Debouncer(FiredCallback onFired) : onFired_(std::move(onFired)) {}

void Debouncer::SetDelay(std::chrono::milliseconds delay) {
    {
        std::lock_guard lock(mutex_);
        delay_ = delay;
    }
    cv_.notify_one();
}

void Debouncer::Arm() {
    {
        std::lock_guard lock(mutex_);
        armed_ = true;
        armedAt_ = Now();
    }
    cv_.notify_one();
}

bool Debouncer::FireNow() {
    {
        std::lock_guard lock(mutex_);
        if (!armed_) {
            return false;
        }
        armed_ = false;
    }
    cv_.notify_one();
    if (onFired_) {
        onFired_();
    }
    return true;
}

void Debouncer::Disarm() {
    {
        std::lock_guard lock(mutex_);
        armed_ = false;
    }
    cv_.notify_one();
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
        std::unique_lock lock(mutex_);
        while (running_) {
            if (!armed_) {
                cv_.wait(lock, [this] { return !running_ || armed_; });
                continue;
            }

            const auto target = armedAt_ + delay_;
            const auto now = Now();
            if (now >= target) {
                armed_ = false;
                lock.unlock();
                if (onFired_) {
                    onFired_();
                }
                lock.lock();
            } else {
                if (nowOverride_.load()) {
                    cv_.wait_for(lock, std::chrono::milliseconds(10));
                } else {
                    cv_.wait_until(lock, target);
                }
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
    cv_.notify_all();
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
}

std::chrono::steady_clock::time_point Debouncer::Now() const {
    if (nowOverride_.load()) {
        return fakeNow_.load();
    }
    return Clock::now();
}

} // namespace aiie