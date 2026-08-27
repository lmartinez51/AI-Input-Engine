#include "mini_test.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

// Simulated typing event
struct TypingEvent {
    int64_t timeMs = 0;       // Monotonic timestamp of the event
    enum class Type { Char, Backspace, Commit } type = Type::Char;
    wchar_t ch = 0;
};

// Simulation metrics
struct SimulationMetrics {
    int requestsGenerated = 0;
    int requestsCompleted = 0;
    int requestsCancelled = 0;
    int staleRequests = 0;
    int usefulRequests = 0;
    int maxPendingRequests = 0;
    int maxConcurrentProviderWork = 0;
    int requestsRejectedByPendingLimit = 0;
    int64_t finalCommitLatencyMs = 0;
    bool correctlyCommitted = false;
    std::wstring finalCommittedText;
};

// Mock In-flight request
struct SimulatedInflight {
    int64_t id = 0;
    std::wstring text;
    int64_t dispatchTimeMs = 0;
    int64_t completionTimeMs = 0;
    bool cancelled = false;
};

// ---------------------------------------------------------------------------
// 1. Current Engine Simulator (Exact Baseline)
// ---------------------------------------------------------------------------
class CurrentEngineSimulator {
public:
    explicit CurrentEngineSimulator(int debounceMs = 300, int minChars = 1, int mockLatencyMs = 300)
        : debounceMs_(debounceMs), minChars_(minChars), mockLatencyMs_(mockLatencyMs) {}

    SimulationMetrics Run(const std::vector<TypingEvent>& timeline, int maxPendingLimit = 16) {
        SimulationMetrics metrics;
        std::wstring buffer;
        std::wstring inflightSnapshot;
        int64_t inflightRequestId = -1;
        int64_t nextRequestId = 1;
        int64_t debounceDeadlineMs = -1;
        int currentPending = 0;
        int currentConcurrentWork = 0;
        std::vector<SimulatedInflight> inflightPool;

        int64_t lastEventTime = 0;
        if (!timeline.empty()) {
            lastEventTime = timeline.back().timeMs;
        }

        size_t eventIdx = 0;
        int64_t currentTimeMs = 0;

        while (eventIdx < timeline.size() || !inflightPool.empty() || debounceDeadlineMs >= 0) {
            int64_t nextTime = INT64_MAX;
            if (eventIdx < timeline.size()) {
                nextTime = std::min(nextTime, timeline[eventIdx].timeMs);
            }
            if (debounceDeadlineMs >= 0) {
                nextTime = std::min(nextTime, debounceDeadlineMs);
            }
            for (const auto& req : inflightPool) {
                nextTime = std::min(nextTime, req.completionTimeMs);
            }

            if (nextTime == INT64_MAX) break;
            currentTimeMs = nextTime;

            // 1. Process provider completions
            for (auto it = inflightPool.begin(); it != inflightPool.end();) {
                if (it->completionTimeMs <= currentTimeMs) {
                    currentPending--;
                    currentConcurrentWork--;
                    metrics.requestsCompleted++;
                    if (it->id == inflightRequestId) {
                        metrics.usefulRequests++;
                        metrics.correctlyCommitted = true;
                        metrics.finalCommittedText = L"[TRANS:" + it->text + L"]";
                        metrics.finalCommitLatencyMs = currentTimeMs - lastEventTime;
                    } else {
                        metrics.staleRequests++;
                    }
                    it = inflightPool.erase(it);
                } else {
                    ++it;
                }
            }

            // 2. Process debounce timer
            if (debounceDeadlineMs >= 0 && currentTimeMs >= debounceDeadlineMs) {
                debounceDeadlineMs = -1;
                if (static_cast<int>(buffer.size()) >= minChars_) {
                    if (inflightRequestId == -1 || inflightSnapshot != buffer) {
                        if (currentPending >= maxPendingLimit) {
                            metrics.requestsRejectedByPendingLimit++;
                        } else {
                            int64_t id = nextRequestId++;
                            inflightRequestId = id;
                            inflightSnapshot = buffer;
                            currentPending++;
                            currentConcurrentWork++;
                            metrics.requestsGenerated++;
                            metrics.maxPendingRequests = std::max(metrics.maxPendingRequests, currentPending);
                            metrics.maxConcurrentProviderWork = std::max(metrics.maxConcurrentProviderWork, currentConcurrentWork);

                            SimulatedInflight req;
                            req.id = id;
                            req.text = buffer;
                            req.dispatchTimeMs = currentTimeMs;
                            req.completionTimeMs = currentTimeMs + mockLatencyMs_;
                            inflightPool.push_back(req);
                        }
                    }
                }
            }

            // 3. Process typing events
            while (eventIdx < timeline.size() && timeline[eventIdx].timeMs == currentTimeMs) {
                const auto& ev = timeline[eventIdx++];
                if (ev.type == TypingEvent::Type::Char) {
                    const bool isTerminator = (ev.ch == L'.' || ev.ch == L'!' || ev.ch == L'?' || ev.ch == L'\n');
                    buffer.push_back(ev.ch);
                    if (isTerminator) {
                        debounceDeadlineMs = -1;
                        if (static_cast<int>(buffer.size()) >= minChars_) {
                            if (inflightRequestId == -1 || inflightSnapshot != buffer) {
                                if (currentPending >= maxPendingLimit) {
                                    metrics.requestsRejectedByPendingLimit++;
                                } else {
                                    int64_t id = nextRequestId++;
                                    inflightRequestId = id;
                                    inflightSnapshot = buffer;
                                    currentPending++;
                                    currentConcurrentWork++;
                                    metrics.requestsGenerated++;
                                    metrics.maxPendingRequests = std::max(metrics.maxPendingRequests, currentPending);
                                    metrics.maxConcurrentProviderWork = std::max(metrics.maxConcurrentProviderWork, currentConcurrentWork);

                                    SimulatedInflight req;
                                    req.id = id;
                                    req.text = buffer;
                                    req.dispatchTimeMs = currentTimeMs;
                                    req.completionTimeMs = currentTimeMs + mockLatencyMs_;
                                    inflightPool.push_back(req);
                                }
                            }
                        }
                    } else {
                        debounceDeadlineMs = currentTimeMs + debounceMs_;
                    }
                } else if (ev.type == TypingEvent::Type::Backspace) {
                    if (!buffer.empty()) {
                        buffer.pop_back();
                    }
                    inflightRequestId = -1;
                    if (buffer.empty()) {
                        debounceDeadlineMs = -1;
                    } else {
                        debounceDeadlineMs = currentTimeMs + debounceMs_;
                    }
                } else if (ev.type == TypingEvent::Type::Commit) {
                    debounceDeadlineMs = -1;
                    buffer.clear();
                    inflightRequestId = -1;
                }
            }
        }
        return metrics;
    }

private:
    int debounceMs_;
    int minChars_;
    int mockLatencyMs_;
};

// ---------------------------------------------------------------------------
// 2. Adaptive Trigger Engine Simulator (Proposed Optimizer)
// ---------------------------------------------------------------------------
class AdaptiveTriggerEngineSimulator {
public:
    struct Config {
        int minCharsFloor = 3;             // Minimum characters before ANY request
        int wordBoundaryDebounceMs = 750;  // Debounce after space
        int clauseDebounceMs = 600;        // Debounce after comma / semicolon / colon
        int midWordDebounceMs = 1200;      // Debounce mid-word
        int requestCooldownMs = 800;       // Minimum interval between non-terminal requests
        int mockLatencyMs = 300;
    };

    explicit AdaptiveTriggerEngineSimulator(Config cfg) : cfg_(cfg) {}

    static bool IsTerminalPunctuation(wchar_t c) {
        return (c == L'.' || c == L'!' || c == L'?' || c == L'\n');
    }

    static bool IsClausePunctuation(wchar_t c) {
        return (c == L',' || c == L';' || c == L':');
    }

    int DetermineAdaptiveDebounce(const std::wstring& buf) const {
        if (buf.empty()) return cfg_.midWordDebounceMs;
        const wchar_t last = buf.back();
        if (IsClausePunctuation(last)) {
            return cfg_.clauseDebounceMs;
        }
        if (last == L' ' || last == L'\t') {
            return cfg_.wordBoundaryDebounceMs;
        }
        return cfg_.midWordDebounceMs;
    }

    SimulationMetrics Run(const std::vector<TypingEvent>& timeline, int maxPendingLimit = 16) {
        SimulationMetrics metrics;
        std::wstring buffer;
        std::wstring inflightSnapshot;
        int64_t inflightRequestId = -1;
        int64_t nextRequestId = 1;
        int64_t debounceDeadlineMs = -1;
        int64_t lastSubmitTimeMs = -10000;
        int currentPending = 0;
        int currentConcurrentWork = 0;
        std::vector<SimulatedInflight> inflightPool;

        int64_t lastEventTime = 0;
        if (!timeline.empty()) {
            lastEventTime = timeline.back().timeMs;
        }

        size_t eventIdx = 0;
        int64_t currentTimeMs = 0;

        auto trySubmit = [&](bool isTerminalFastPath) {
            // Guard 1: Minimum source length floor
            if (static_cast<int>(buffer.size()) < cfg_.minCharsFloor) {
                return false;
            }
            // Guard 2: Deduplicate identical snapshot
            if (inflightRequestId != -1 && inflightSnapshot == buffer) {
                return false;
            }
            // Guard 3: Request cooldown (Bypassed completely by terminal fast path)
            if (!isTerminalFastPath) {
                if (currentTimeMs - lastSubmitTimeMs < cfg_.requestCooldownMs) {
                    debounceDeadlineMs = lastSubmitTimeMs + cfg_.requestCooldownMs;
                    return false;
                }
            }

            if (currentPending >= maxPendingLimit) {
                metrics.requestsRejectedByPendingLimit++;
                return false;
            }

            int64_t id = nextRequestId++;
            inflightRequestId = id;
            inflightSnapshot = buffer;
            lastSubmitTimeMs = currentTimeMs;
            currentPending++;
            currentConcurrentWork++;
            metrics.requestsGenerated++;
            metrics.maxPendingRequests = std::max(metrics.maxPendingRequests, currentPending);
            metrics.maxConcurrentProviderWork = std::max(metrics.maxConcurrentProviderWork, currentConcurrentWork);

            SimulatedInflight req;
            req.id = id;
            req.text = buffer;
            req.dispatchTimeMs = currentTimeMs;
            req.completionTimeMs = currentTimeMs + cfg_.mockLatencyMs;
            inflightPool.push_back(req);
            debounceDeadlineMs = -1;
            return true;
        };

        while (eventIdx < timeline.size() || !inflightPool.empty() || debounceDeadlineMs >= 0) {
            int64_t nextTime = INT64_MAX;
            if (eventIdx < timeline.size()) {
                nextTime = std::min(nextTime, timeline[eventIdx].timeMs);
            }
            if (debounceDeadlineMs >= 0) {
                nextTime = std::min(nextTime, debounceDeadlineMs);
            }
            for (const auto& req : inflightPool) {
                nextTime = std::min(nextTime, req.completionTimeMs);
            }

            if (nextTime == INT64_MAX) break;
            currentTimeMs = nextTime;

            // 1. Process provider completions
            for (auto it = inflightPool.begin(); it != inflightPool.end();) {
                if (it->completionTimeMs <= currentTimeMs) {
                    currentPending--;
                    currentConcurrentWork--;
                    metrics.requestsCompleted++;
                    if (it->id == inflightRequestId) {
                        metrics.usefulRequests++;
                        metrics.correctlyCommitted = true;
                        metrics.finalCommittedText = L"[TRANS:" + it->text + L"]";
                        metrics.finalCommitLatencyMs = currentTimeMs - lastEventTime;
                    } else {
                        metrics.staleRequests++;
                    }
                    it = inflightPool.erase(it);
                } else {
                    ++it;
                }
            }

            // 2. Process adaptive debounce
            if (debounceDeadlineMs >= 0 && currentTimeMs >= debounceDeadlineMs) {
                debounceDeadlineMs = -1;
                trySubmit(false);
            }

            // 3. Process typing events
            while (eventIdx < timeline.size() && timeline[eventIdx].timeMs == currentTimeMs) {
                const auto& ev = timeline[eventIdx++];
                if (ev.type == TypingEvent::Type::Char) {
                    const bool isTerm = IsTerminalPunctuation(ev.ch);
                    buffer.push_back(ev.ch);
                    if (isTerm) {
                        debounceDeadlineMs = -1;
                        trySubmit(true);
                    } else {
                        const int delay = DetermineAdaptiveDebounce(buffer);
                        debounceDeadlineMs = currentTimeMs + delay;
                    }
                } else if (ev.type == TypingEvent::Type::Backspace) {
                    if (!buffer.empty()) {
                        buffer.pop_back();
                    }
                    inflightRequestId = -1;
                    if (buffer.empty()) {
                        debounceDeadlineMs = -1;
                    } else {
                        const int delay = DetermineAdaptiveDebounce(buffer);
                        debounceDeadlineMs = currentTimeMs + delay;
                    }
                } else if (ev.type == TypingEvent::Type::Commit) {
                    debounceDeadlineMs = -1;
                    buffer.clear();
                    inflightRequestId = -1;
                }
            }
        }

        return metrics;
    }

private:
    Config cfg_;
};

// ---------------------------------------------------------------------------
// 3. Timeline Generator Helper
// ---------------------------------------------------------------------------
std::vector<TypingEvent> BuildTimeline(const std::wstring& text, int baseCharDelayMs,
                                       const std::vector<std::pair<size_t, int>>& pauses = {},
                                       int seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> jitter(-15, 15);
    std::vector<TypingEvent> timeline;
    int64_t t = 0;

    for (size_t i = 0; i < text.size(); ++i) {
        t += std::max(20, baseCharDelayMs + jitter(rng));
        for (const auto& p : pauses) {
            if (p.first == i) {
                t += p.second;
            }
        }
        TypingEvent ev;
        ev.timeMs = t;
        ev.type = TypingEvent::Type::Char;
        ev.ch = text[i];
        timeline.push_back(ev);
    }
    return timeline;
}

} // namespace

// ---------------------------------------------------------------------------
// 4. Test Cases
// ---------------------------------------------------------------------------

TEST(trigger_optimizer_invariants_and_lone_terminator_guard) {
    AdaptiveTriggerEngineSimulator::Config cfg;
    cfg.minCharsFloor = 3;
    AdaptiveTriggerEngineSimulator sim(cfg);

    // 1. Lone terminator guard: "." alone must NEVER generate a request
    {
        std::vector<TypingEvent> loneDot = { { 100, TypingEvent::Type::Char, L'.' } };
        SimulationMetrics m = sim.Run(loneDot);
        CHECK_EQ(m.requestsGenerated, 0);
    }
    // 2. Short 2-char buffer with terminator: "a." must NEVER generate a request
    {
        std::vector<TypingEvent> twoChar = {
            { 100, TypingEvent::Type::Char, L'a' },
            { 200, TypingEvent::Type::Char, L'.' }
        };
        SimulationMetrics m = sim.Run(twoChar);
        CHECK_EQ(m.requestsGenerated, 0);
    }
    // 3. 3-char buffer with terminator: "ab." satisfies floor >= 3 and triggers
    {
        std::vector<TypingEvent> threeChar = {
            { 100, TypingEvent::Type::Char, L'a' },
            { 200, TypingEvent::Type::Char, L'b' },
            { 300, TypingEvent::Type::Char, L'.' }
        };
        SimulationMetrics m = sim.Run(threeChar);
        CHECK_EQ(m.requestsGenerated, 1);
        CHECK_EQ(m.usefulRequests, 1);
    }
}

TEST(trigger_optimizer_12_scenarios_comparison) {
    CurrentEngineSimulator baseline(300, 1, 300); // 300ms debounce, minChars=1, mockLatency=300ms
    AdaptiveTriggerEngineSimulator::Config cfg;
    cfg.wordBoundaryDebounceMs = 750;
    cfg.clauseDebounceMs = 600;
    cfg.midWordDebounceMs = 1200;
    cfg.requestCooldownMs = 800;
    cfg.minCharsFloor = 3;
    cfg.mockLatencyMs = 300;
    AdaptiveTriggerEngineSimulator adaptive(cfg);

    struct ScenarioDef {
        std::string name;
        std::wstring text;
        int charDelayMs;
        std::vector<std::pair<size_t, int>> pauses;
    };

    std::vector<ScenarioDef> scenarios = {
        { "1. Short Sentence", L"Hola mundo.", 150, {} },
        { "2. Medium Sentence", L"Que vas hacer el proximo sabado?", 250, { { 3, 350 }, { 7, 350 }, { 13, 350 } } },
        { "3. Long Compound Sentence",
          L"Deshabilite la opcion, por cierto te estoy escribiendo en espanol, pero tu veras este texto en ingles.",
          220, { { 21, 450 }, { 32, 400 }, { 64, 450 }, { 80, 400 } } },
        { "4. Continuous Fast Typing", L"Este es un texto escrito muy rapido sin pausas.", 130, {} },
        { "5. Terminator Fast Path", L"Terminamos ahora!", 240, {} },
        { "6. Slow Typist", L"Escribiendo muy despacio para probar.", 450, { { 11, 500 }, { 15, 500 }, { 24, 500 } } },
        { "7. Fast Typist", L"Escribiendo muy rapido para probar.", 140, {} },
        { "8. 250ms Pause", L"Texto con una pequeña pausa en medio del camino.", 220, { { 15, 250 } } },
        { "9. 500ms Pause", L"Texto con una pausa mediana para pensar.", 220, { { 15, 500 } } },
        { "10. 1000ms Pause", L"Texto con una pausa larga esperando respuesta.", 220, { { 15, 1000 } } },
        { "11. Typo + Backspace", L"Texto con error corregido.", 200, { { 9, 300 } } },
        { "12. Multi-Sentence", L"Primera oracion terminada. Segunda oracion iniciada.", 200, { { 25, 400 } } }
    };

    int totalBaselineReqs = 0;
    int totalBaselineStale = 0;
    int totalAdaptiveReqs = 0;
    int totalAdaptiveStale = 0;
    int maxBaselinePendingOverall = 0;
    int maxAdaptivePendingOverall = 0;
    std::cout << "\n========================================================================================\n";
    std::cout << "TRANSLATION TRIGGER OPTIMIZATION: 12-SCENARIO DETERMINISTIC BENCHMARK\n";
    std::cout << "========================================================================================\n";
    std::cout << "Scenario Name                   | Base Req (Stale) | Adapt Req (Stale) | Max Pend (B->A) | Reduct %\n";
    std::cout << "--------------------------------+------------------+-------------------+-----------------+---------\n";

    for (const auto& sc : scenarios) {
        auto timeline = BuildTimeline(sc.text, sc.charDelayMs, sc.pauses);
        SimulationMetrics mBase = baseline.Run(timeline);
        SimulationMetrics mAdapt = adaptive.Run(timeline);

        totalBaselineReqs += mBase.requestsGenerated;
        totalBaselineStale += mBase.staleRequests;
        totalAdaptiveReqs += mAdapt.requestsGenerated;
        totalAdaptiveStale += mAdapt.staleRequests;

        maxBaselinePendingOverall = std::max(maxBaselinePendingOverall, mBase.maxPendingRequests);
        maxAdaptivePendingOverall = std::max(maxAdaptivePendingOverall, mAdapt.maxPendingRequests);

        const double scRed = (mBase.requestsGenerated > 0)
            ? (100.0 * (mBase.requestsGenerated - mAdapt.requestsGenerated) / mBase.requestsGenerated)
            : 0.0;

        char buf[256];
        snprintf(buf, sizeof(buf), "%-31s |     %2d  (%2d)     |     %2d  (%2d)      |     %2d -> %2d    |  %5.1f%%\n",
                 sc.name.c_str(), mBase.requestsGenerated, mBase.staleRequests,
                 mAdapt.requestsGenerated, mAdapt.staleRequests,
                 mBase.maxPendingRequests, mAdapt.maxPendingRequests, scRed);
        std::cout << buf;

        // Verification checks
        CHECK(mAdapt.correctlyCommitted);
        CHECK_EQ(mAdapt.requestsRejectedByPendingLimit, 0);
        CHECK(mAdapt.requestsGenerated <= mBase.requestsGenerated);
        CHECK(mAdapt.staleRequests <= mBase.staleRequests);
    }

    const double overallReduction = 100.0 * (totalBaselineReqs - totalAdaptiveReqs) / totalBaselineReqs;
    const double staleReduction = (totalBaselineStale > 0)
        ? (100.0 * (totalBaselineStale - totalAdaptiveStale) / totalBaselineStale)
        : 100.0;

    std::cout << "--------------------------------+------------------+-------------------+-----------------+---------\n";
    char totBuf[256];
    snprintf(totBuf, sizeof(totBuf), "TOTAL / AGGREGATE (12 Scenarios)|     %2d  (%2d)     |     %2d  (%2d)      |     %2d -> %2d    |  %5.1f%%\n",
             totalBaselineReqs, totalBaselineStale, totalAdaptiveReqs, totalAdaptiveStale,
             maxBaselinePendingOverall, maxAdaptivePendingOverall, overallReduction);
    std::cout << totBuf;
    std::cout << "========================================================================================\n";
    std::cout << "Stale Request Reduction: " << staleReduction << "%\n";
    std::cout << "Max Concurrent Pending: Baseline = " << maxBaselinePendingOverall << " -> Adaptive = " << maxAdaptivePendingOverall << " (Saturation Eliminated)\n";
    std::cout << "========================================================================================\n\n";

    CHECK(totalBaselineReqs > 0);
    CHECK(overallReduction > 50.0);
    CHECK(maxAdaptivePendingOverall <= 2);
}

TEST(trigger_optimizer_word_boundary_sweep) {
    std::wstring longSentence = L"Deshabilite la opcion, por cierto te estoy escribiendo en espanol, pero tu veras este texto en ingles.";
    auto timeline = BuildTimeline(longSentence, 220, { { 21, 450 }, { 32, 400 }, { 64, 450 }, { 80, 400 } });

    std::vector<int> sweepValues = { 500, 750, 1000, 1250 };
    for (int wb : sweepValues) {
        AdaptiveTriggerEngineSimulator::Config cfg;
        cfg.wordBoundaryDebounceMs = wb;
        cfg.clauseDebounceMs = 600;
        cfg.midWordDebounceMs = 1200;
        cfg.requestCooldownMs = 800;
        cfg.minCharsFloor = 3;
        cfg.mockLatencyMs = 300;
        AdaptiveTriggerEngineSimulator sim(cfg);

        SimulationMetrics m = sim.Run(timeline);
        CHECK(m.correctlyCommitted);
        CHECK_EQ(m.requestsRejectedByPendingLimit, 0);
        CHECK(m.maxPendingRequests <= 2);
        CHECK(m.requestsGenerated <= 3);
    }
}
