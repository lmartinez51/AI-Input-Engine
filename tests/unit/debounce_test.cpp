#include "mini_test.h"

#include "Translation/Debouncer.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace aiie;

TEST(debouncer_fires_after_delay) {
    std::atomic<int> fired{0};
    Debouncer d([&fired] { fired.fetch_add(1); });
    d.SetDelay(std::chrono::milliseconds(30));
    d.Arm();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(d.Tick());
    CHECK_EQ(fired.load(), 1);
}

TEST(debouncer_does_not_fire_before_delay) {
    std::atomic<int> fired{0};
    Debouncer d([&fired] { fired.fetch_add(1); });
    d.SetDelay(std::chrono::milliseconds(200));
    d.Arm();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!d.Tick());
    CHECK_EQ(fired.load(), 0);
    d.Disarm();
}

TEST(debouncer_rearm_extends_window) {
    std::atomic<int> fired{0};
    Debouncer d([&fired] { fired.fetch_add(1); });
    d.SetDelay(std::chrono::milliseconds(60));
    d.Arm();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    d.Arm(); // re-arm: idle window restarts
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(!d.Tick()); // only 40ms since last Arm
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(d.Tick());
    CHECK_EQ(fired.load(), 1);
}

TEST(debouncer_fire_now) {
    std::atomic<int> fired{0};
    Debouncer d([&fired] { fired.fetch_add(1); });
    d.SetDelay(std::chrono::hours(1));
    CHECK(!d.FireNow()); // not armed
    d.Arm();
    CHECK(d.FireNow());
    CHECK_EQ(fired.load(), 1);
    CHECK(!d.FireNow()); // disarmed
}

TEST(debouncer_disarm_suppresses) {
    std::atomic<int> fired{0};
    Debouncer d([&fired] { fired.fetch_add(1); });
    d.SetDelay(std::chrono::milliseconds(30));
    d.Arm();
    d.Disarm();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(!d.Tick());
    CHECK_EQ(fired.load(), 0);
}

TEST(debouncer_start_stop) {
    std::atomic<int> fired{0};
    Debouncer d([&fired] { fired.fetch_add(1); });
    d.SetDelay(std::chrono::milliseconds(20));
    d.Start();
    d.Arm();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK_GT(fired.load(), 0);
    d.Stop();
}

TEST(debouncer_fake_clock) {
    std::atomic<int> fired{0};
    Debouncer d([&fired] { fired.fetch_add(1); });
    d.SetDelay(std::chrono::milliseconds(500));
    auto t0 = std::chrono::steady_clock::now();
    d.SetNowForTest(t0);
    d.Arm();
    CHECK(!d.Tick());
    d.SetNowForTest(t0 + std::chrono::milliseconds(501));
    CHECK(d.Tick());
    CHECK_EQ(fired.load(), 1);
    d.ClearNowForTest();
}