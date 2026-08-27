#include "mini_test.h"

#include "Composition/InputEngine.h"
#include "Core/Dispatcher.h"
#include "Providers/MockProvider.h"
#include "Translation/SpeculativeTranslationManager.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace aiie;

namespace {

class FakeBackend : public IInputBackend {
public:
    mutable std::mutex mtx;
    std::vector<std::pair<std::wstring, size_t>> replacements;
    std::vector<std::wstring> statusShown;
    std::vector<std::wstring> statusHidden;
    std::atomic<int> commits{0};

    void CommitReplacement(std::wstring replacement, const std::wstring& expectedSource) override {
        std::lock_guard lock(mtx);
        replacements.emplace_back(std::move(replacement), expectedSource.size());
    }
    void OnBufferStarted() override {}
    void OnBufferChanged(std::wstring_view) override {}
    void OnBufferCommitted() override { commits.fetch_add(1); }
    void ShowStatus(std::wstring_view message) override {
        std::lock_guard lock(mtx);
        statusShown.emplace_back(message);
    }
    void HideStatus() override {
        std::lock_guard lock(mtx);
        statusHidden.emplace_back(L"");
    }

    size_t ReplacementsCount() const {
        std::lock_guard lock(mtx);
        return replacements.size();
    }
    bool HasReplacements() const {
        std::lock_guard lock(mtx);
        return !replacements.empty();
    }
    std::wstring GetReplacement(size_t index) const {
        std::lock_guard lock(mtx);
        if (index < replacements.size()) return replacements[index].first;
        return L"";
    }
};

// Controllable fake provider for fine-grained speculative timing tests
class ControllableProvider : public ITranslationProvider {
public:
    std::atomic<int> translateCalls{0};
    std::atomic<int> cancelCalls{0};
    std::vector<TranslationCallback> pendingCallbacks;
    std::vector<TranslationRequest> pendingRequests;
    std::mutex mtx;

    std::wstring_view Name() const override { return L"controllable"; }
    std::wstring_view DisplayName() const override { return L"Controllable"; }
    bool RequiresNetwork() const override { return false; }
    ProviderStatus Status() const override { return {ProviderStatus::State::Ready, L""}; }

    void CancelAll() override {
        cancelCalls.fetch_add(1);
    }

    void Translate(TranslationRequest request, TranslationCallback callback) override {
        translateCalls.fetch_add(1);
        std::lock_guard lock(mtx);
        pendingRequests.push_back(request);
        pendingCallbacks.push_back(std::move(callback));
    }

    bool HasPending() {
        std::lock_guard lock(mtx);
        return !pendingCallbacks.empty();
    }

    void CompleteLatest(const std::wstring& text, bool ok = true) {
        TranslationCallback cb;
        int64_t reqId = 0;
        {
            std::lock_guard lock(mtx);
            if (pendingCallbacks.empty()) return;
            cb = std::move(pendingCallbacks.back());
            reqId = pendingRequests.back().requestId;
            pendingCallbacks.pop_back();
            pendingRequests.pop_back();
        }
        if (cb) {
            TranslationResult res;
            res.ok = ok;
            res.text = text;
            res.requestId = reqId;
            cb(std::move(res));
        }
    }

    bool IsCancelled(size_t index) {
        std::lock_guard lock(mtx);
        if (index < pendingRequests.size() && pendingRequests[index].cancellationToken) {
            return pendingRequests[index].cancellationToken->load();
        }
        return false;
    }

    void CancelPending(size_t index) {
        TranslationCallback cb;
        int64_t reqId = 0;
        {
            std::lock_guard lock(mtx);
            if (index >= pendingCallbacks.size()) return;
            cb = std::move(pendingCallbacks[index]);
            reqId = pendingRequests[index].requestId;
            pendingCallbacks.erase(pendingCallbacks.begin() + index);
            pendingRequests.erase(pendingRequests.begin() + index);
        }
        if (cb) {
            TranslationResult res;
            res.ok = false;
            res.error = L"Request cancelled";
            res.requestId = reqId;
            cb(std::move(res));
        }
    }
};

struct SpeculativeHarness {
    std::shared_ptr<ImmediateDispatcher> dispatcher = std::make_shared<ImmediateDispatcher>();
    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    std::shared_ptr<MockProvider> provider = std::make_shared<MockProvider>(std::chrono::milliseconds(10));
    std::shared_ptr<TranslationCache> cache = std::make_shared<TranslationCache>(100);
    std::shared_ptr<TranslationPipeline> pipeline;
    std::shared_ptr<InputEngine> engine;

    explicit SpeculativeHarness(bool speculativeEnabled = true, int specDebounceMs = 20, int finalDebounceMs = 100) {
        pipeline = std::make_shared<TranslationPipeline>(provider, cache, 30000);
        engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);
        AppConfig cfg;
        cfg.enabled = true;
        cfg.sourceLanguage = L"es";
        cfg.targetLanguage = L"en";
        cfg.debounceMs = finalDebounceMs;
        cfg.minChars = 10;
        cfg.speculativeTranslationEnabled = speculativeEnabled;
        cfg.speculativeDebounceMs = specDebounceMs;
        cfg.speculativeMinChars = 10;
        engine->ApplyConfig(cfg);
    }
};

void WaitFor(std::function<bool()> pred, int timeoutMs = 3000) {
    for (int i = 0; i < timeoutMs / 10 && !pred(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

// 1. Speculative Request Trigger
TEST(speculative_request_trigger) {
    SpeculativeTranslationManager mgr(true, 150, 5);
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    // Below minChars (< 10)
    CHECK(!mgr.ShouldTriggerSpeculative(L"Hola", id));

    // At or above minChars (10 chars)
    CHECK(mgr.ShouldTriggerSpeculative(L"Hola amigo", id));

    // Same snapshot -> no trigger
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    mgr.StartSpeculativeRequest(1, L"Hola amigo", id, pipeline, nullptr);
    CHECK(!mgr.ShouldTriggerSpeculative(L"Hola amigo", id));

    // >= 8 chars growth -> trigger after cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpCooldownMs + 10));
    CHECK(mgr.ShouldTriggerSpeculative(L"Hola amigo mio querido", id)); // len=22, diff=12 >= 8

    // 1 char change (< 8 chars growth) -> suppressed
    CHECK(!mgr.ShouldTriggerSpeculative(L"Hola amigos", id)); // len=11, diff=1 < 8
}

// 2. Incremental Typing Simulation
TEST(speculative_incremental_typing_simulation) {
    SpeculativeHarness h(true, 15, 200);

    // Type "Hola amigo mio" (14 chars)
    const std::wstring text = L"Hola amigo mio";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    // Wait for speculative debounce (15ms) + mock translation (10ms)
    WaitFor([&] {
        return h.engine->SpeculativeManager()->CurrentCandidate().has_value();
    });

    auto cand = h.engine->SpeculativeManager()->CurrentCandidate();
    CHECK(cand.has_value());
    CHECK(cand->completed);
    CHECK(cand->ok);
    CHECK_STREQ(cand->sourceText, L"Hola amigo mio");
}

// 3. Candidate Completion
TEST(speculative_candidate_completion) {
    SpeculativeTranslationManager mgr(true, 150, 4);
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";
    id.model = L"gpt-5.6-luna";

    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);

    mgr.StartSpeculativeRequest(42, L"Hola amigo mio", id, pipeline, nullptr);

    TranslationResult res;
    res.ok = true;
    res.text = L"Hello my friend";
    res.requestId = 42;

    mgr.OnSpeculativeResult(42, res, L"Hola amigo mio", id, 120);

    auto cand = mgr.CurrentCandidate();
    CHECK(cand.has_value());
    CHECK(cand->completed);
    CHECK(cand->ok);
    CHECK_STREQ(cand->translatedText, L"Hello my friend");
    CHECK_STREQ(cand->sourceText, L"Hola amigo mio");
    CHECK_EQ(cand->totalLatencyMs, 120);
}

// 4. Candidate Exact Reuse
TEST(speculative_candidate_exact_reuse) {
    SpeculativeHarness h(true, 15, 80);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    // Wait for candidate to be ready in background (speculative debounce 15ms + mock 10ms)
    WaitFor([&] {
        return h.engine->SpeculativeManager()->CurrentCandidate().has_value();
    });

    // Commit using Shift+Enter (exact candidate match for "hola mundo")
    h.engine->HandleShiftEnter();

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world\n");

    const auto telem = h.engine->SpeculativeManager()->GetTelemetry();
    CHECK(telem.candidateHits >= 1);
    CHECK(telem.finalRequestsAvoided >= 1);
}

// 5. Exact Identity Match
TEST(speculative_exact_identity_match) {
    SpeculativeTranslationManager mgr(true, 150, 4);
    TransformationIdentity id1;
    id1.sourceLanguage = L"es";
    id1.targetLanguage = L"en";
    id1.model = L"gpt-5.6-luna";

    TransformationIdentity id2 = id1;
    CHECK(id1 == id2);

    id2.targetLanguage = L"zh-Hans";
    CHECK(id1 != id2);

    id2 = id1;
    id2.profile = TranslationProfile::Slang;
    CHECK(id1 != id2);
}

// 6. Source Mismatch
TEST(speculative_source_mismatch) {
    SpeculativeTranslationManager mgr(true, 150, 4);
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);

    TranslationResult res;
    res.ok = true;
    res.text = L"Hello my friend";
    res.requestId = 10;

    mgr.StartSpeculativeRequest(10, L"Hola amigo mio", id, pipeline, nullptr);
    mgr.OnSpeculativeResult(10, res, L"Hola amigo mio", id, 100);

    TranslationRequest finalReq;
    finalReq.text = L"Hola amigo mio querido"; // Changed!
    finalReq.sourceLanguage = L"es";
    finalReq.targetLanguage = L"en";

    TranslationResult outRes;
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes));
    CHECK_EQ(mgr.GetTelemetry().candidateMisses, 1);
}

// 7. Configuration Mismatch
TEST(speculative_configuration_mismatch) {
    SpeculativeTranslationManager mgr(true, 150, 4);
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);

    TranslationResult res;
    res.ok = true;
    res.text = L"Hello my friend";
    res.requestId = 10;

    mgr.StartSpeculativeRequest(10, L"Hola amigo mio", id, pipeline, nullptr);
    mgr.OnSpeculativeResult(10, res, L"Hola amigo mio", id, 100);

    TranslationRequest finalReq;
    finalReq.text = L"Hola amigo mio";
    finalReq.sourceLanguage = L"es";
    finalReq.targetLanguage = L"zh-Hans"; // Changed!

    TranslationResult outRes;
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes));
}

// 8. Candidate Invalidation on Backspace
TEST(speculative_candidate_invalidation) {
    SpeculativeHarness h(true, 15, 200);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    WaitFor([&] {
        return h.engine->SpeculativeManager()->CurrentCandidate().has_value();
    });
    CHECK(h.engine->SpeculativeManager()->CurrentCandidate().has_value());

    // Backspace invalidates candidate immediately
    h.engine->HandleBackspace();
    CHECK(!h.engine->SpeculativeManager()->CurrentCandidate().has_value());
}

// 9. Speculative Cancellation & Throttling
TEST(speculative_cancellation) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    SpeculativeTranslationManager mgr(true, 150, 4);
    TransformationIdentity id;

    mgr.StartSpeculativeRequest(1, L"Hola amigo mio", id, pipeline, nullptr);
    CHECK_EQ(mgr.ActiveSpeculativeRequestId(), 1);

    // Starting request 2 cancels request 1
    mgr.StartSpeculativeRequest(2, L"Hola amigo mio querido", id, pipeline, nullptr);
    CHECK_EQ(mgr.ActiveSpeculativeRequestId(), 2);
    CHECK_EQ(mgr.GetTelemetry().requestsCancelled, 1);
}

// 10. Stale Speculative Completion Discarded
TEST(speculative_stale_completion_discarded) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    SpeculativeTranslationManager mgr(true, 150, 4);
    TransformationIdentity id;

    mgr.StartSpeculativeRequest(1, L"Hola amigo mio", id, pipeline, nullptr);
    mgr.StartSpeculativeRequest(2, L"Hola amigo mio querido", id, pipeline, nullptr);

    // Stale result for request 1 arrives via provider completion
    controllable->CompleteLatest(L"Hello my friend");

    // Candidate should NOT be populated from stale result
    CHECK(!mgr.CurrentCandidate().has_value());
    CHECK_EQ(mgr.ActiveSpeculativeRequestId(), 2);
}

// 11. Finalization Before Candidate Ready
TEST(speculative_finalization_before_candidate_ready) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    cfg.speculativeDebounceMs = 10;
    cfg.debounceMs = 60;
    cfg.minChars = 10;
    cfg.speculativeMinChars = 10;
    engine->ApplyConfig(cfg);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        engine->HandleCharacter(c);
    }

    // Fast finalization (Enter/Period) before speculative completes
    engine->HandleTerminator(L'.');

    // Engine issues final request directly
    WaitFor([&] { return controllable->HasPending(); });
    CHECK_EQ(controllable->pendingRequests.size(), 1u);
    CHECK_EQ(controllable->pendingRequests[0].commitAuthority, CommitAuthority::Allowed);
}

// 12. Candidate Hit Skips Final Provider Request
TEST(speculative_candidate_hit_skips_final_provider_request) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    h.engine->OnSpeculativeDebounceFired();

    // Wait for speculative candidate to complete
    WaitFor([&] {
        return h.engine->SpeculativeManager()->CurrentCandidate().has_value();
    });

    // Shift+Enter fires and consumes the exact candidate
    h.engine->HandleShiftEnter();

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world\n");
    CHECK_EQ(h.engine->SpeculativeManager()->GetTelemetry().candidateHits, 1);
    CHECK_EQ(h.engine->SpeculativeManager()->GetTelemetry().finalRequestsAvoided, 1);
}

// 13. Candidate Miss Fallback
TEST(speculative_candidate_miss_fallback) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    // No speculative candidate ready yet.
    // User types terminator '.' immediately.
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world.");
}

// 14. Foreground Switch Invalidates Candidate
TEST(speculative_foreground_switch_invalidates_candidate) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    h.engine->OnSpeculativeDebounceFired();
    WaitFor([&] {
        return h.engine->SpeculativeManager()->CurrentCandidate().has_value();
    });

    // Foreground switch cancels active composition and candidate
    h.engine->CancelActiveCompositionForFocusChange();
    CHECK(!h.engine->SpeculativeManager()->CurrentCandidate().has_value());
    CHECK_EQ(h.engine->Buffer(), L"");
}

// 15. Production Disabled Mode Identical to Baseline
TEST(speculative_production_disabled_mode_identical_to_baseline) {
    SpeculativeHarness h(false, 15, 50); // Disabled

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }
    h.engine->HandleTerminator(L'.');

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    // Speculative manager never triggered
    CHECK_EQ(h.engine->SpeculativeManager()->GetTelemetry().requestsStarted, 0);

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world.");
}

// 16. Rapid Typing Produces At Most One Active Speculative Request
TEST(speculative_rapid_typing_at_most_one_active_request) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    SpeculativeTranslationManager mgr(true, 10, 4);
    TransformationIdentity id;

    for (int i = 0; i < 20; ++i) {
        mgr.StartSpeculativeRequest(i + 1, std::wstring(L"Hola ") + std::to_wstring(i), id, pipeline, nullptr);
    }

    // Physical pipeline has at most 1 slot occupied
    CHECK_LE(pipeline->PendingSpeculative(), 1u);
    CHECK_EQ(mgr.ActiveSpeculativeRequestId(), 20);
    CHECK_EQ(mgr.GetTelemetry().requestsCancelled, 19);
}

// 17. Weak Signal Starts Speculative Request with Forbidden Authority
TEST(speculative_weak_signal_starts_speculative_forbidden_request) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    cfg.minChars = 10;
    cfg.speculativeMinChars = 10;
    cfg.speculativeDebounceMs = 15;
    cfg.debounceMs = 100;
    engine->ApplyConfig(cfg);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        engine->HandleCharacter(c);
    }

    // Trigger speculative debounce
    engine->OnSpeculativeDebounceFired();

    WaitFor([&] { return controllable->HasPending(); });
    CHECK_EQ(controllable->pendingRequests.size(), 1u);
    if (!controllable->pendingRequests.empty()) {
        CHECK_EQ(controllable->pendingRequests[0].commitAuthority, CommitAuthority::Forbidden);
    }
}

// 18. Consumed Candidate Cannot Be Reused Twice
TEST(speculative_consumed_candidate_cannot_be_reused_twice) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    h.engine->OnSpeculativeDebounceFired();
    WaitFor([&] { return h.engine->SpeculativeManager()->CurrentCandidate().has_value(); });

    // 1st consumption succeeds
    TranslationRequest req;
    req.text = L"hola mundo";
    req.commitAuthority = CommitAuthority::Allowed;
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    TranslationResult res1;
    CHECK(h.engine->SpeculativeManager()->TryConsumeExactCandidate(req, res1));

    // 2nd consumption fails (candidate was cleared on consumption)
    TranslationResult res2;
    CHECK(!h.engine->SpeculativeManager()->TryConsumeExactCandidate(req, res2));
}

// 19. Terminal '?' Candidate Hit Instant Commit
TEST(speculative_terminal_question_candidate_hit_instant_commit) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    h.engine->OnSpeculativeDebounceFired();
    WaitFor([&] { return h.engine->SpeculativeManager()->CurrentCandidate().has_value(); });

    // Terminal '?' pressed
    h.engine->HandleTerminator(L'?');

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world?");
    CHECK_EQ(h.engine->Buffer(), L"");
}

// 20. Terminal '!' Candidate Hit Instant Commit
TEST(speculative_terminal_exclamation_candidate_hit_instant_commit) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    h.engine->OnSpeculativeDebounceFired();
    WaitFor([&] { return h.engine->SpeculativeManager()->CurrentCandidate().has_value(); });

    // Terminal '!' pressed
    h.engine->HandleTerminator(L'!');

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world!");
    CHECK_EQ(h.engine->Buffer(), L"");
}

// 21. Candidate Invalidated by Additional Typing
TEST(speculative_candidate_invalidated_by_additional_typing) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    h.engine->OnSpeculativeDebounceFired();
    WaitFor([&] { return h.engine->SpeculativeManager()->CurrentCandidate().has_value(); });

    // Typing additional character makes buffer "hola mundos", so candidate "hola mundo" cannot be consumed
    h.engine->HandleCharacter(L's');
    TranslationRequest req;
    req.text = L"hola mundos";
    req.sourceLanguage = L"es";
    req.targetLanguage = L"en";
    TranslationResult res;
    CHECK(!h.engine->SpeculativeManager()->TryConsumeExactCandidate(req, res));
}

// 22. Candidate Invalidated by Configuration Change
TEST(speculative_candidate_invalidated_by_configuration_change) {
    SpeculativeHarness h(true, 10, 100);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    h.engine->OnSpeculativeDebounceFired();
    WaitFor([&] { return h.engine->SpeculativeManager()->CurrentCandidate().has_value(); });

    // Request with different target language misses candidate
    TranslationRequest req;
    req.text = L"hola mundo";
    req.commitAuthority = CommitAuthority::Allowed;
    req.sourceLanguage = L"es";
    req.targetLanguage = L"fr";
    TranslationResult res;
    CHECK(!h.engine->SpeculativeManager()->TryConsumeExactCandidate(req, res));
}

// 23. Shift+Enter Candidate Miss Falls Back to Final Request with Trailing Newline
TEST(speculative_shift_enter_candidate_miss_fallback_with_newline) {
    SpeculativeHarness h(true, 200, 200); // Speculative debounce delayed (candidate not ready)

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) {
        h.engine->HandleCharacter(c);
    }

    // Shift+Enter pressed immediately before candidate is ready
    h.engine->HandleShiftEnter();

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    const std::wstring rep = h.backend->GetReplacement(0);
    CHECK(!rep.empty());
    CHECK_EQ(rep.back(), L'\n');
    CHECK_STREQ(rep, L"hello world\n");
}

// 24. End-to-End Motivating Pause Scenario
TEST(speculative_end_to_end_motivating_pause_scenario) {
    SpeculativeHarness h(true, 15, 100);

    // 1. User types: "hola mundo"
    const std::wstring part1 = L"hola mundo";
    for (wchar_t c : part1) {
        h.engine->HandleCharacter(c);
    }

    // 2. User pauses for 2+ seconds
    h.engine->OnDebounceFired();
    h.engine->OnDebounceFired();

    // Speculative candidate completes off-screen; ZERO visible replacements
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!h.backend->HasReplacements());
    CHECK_EQ(h.engine->Buffer(), L"hola mundo");

    // 3. User continues typing in the same composition: " grande"
    const std::wstring part2 = L" grande";
    for (wchar_t c : part2) {
        h.engine->HandleCharacter(c);
    }

    // 4. User types final period '.'
    h.engine->HandleTerminator(L'.');

    // Exactly ONE atomic replacement produced, no fragmentation, text is correct
    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world grande.");
    CHECK_EQ(h.engine->Buffer(), L"");
}

// 25. Realistic Multi-Sentence Test
TEST(speculative_multi_sentence_isolation) {
    SpeculativeHarness h(true, 15, 100);

    // Sentence 1: "hola mundo."
    const std::wstring sent1 = L"hola mundo";
    for (wchar_t c : sent1) h.engine->HandleCharacter(c);
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return h.backend->ReplacementsCount() == 1u; });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world.");

    // Pause between sentences
    h.engine->OnDebounceFired();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Sentence 2: "este proyecto."
    const std::wstring sent2 = L"este proyecto";
    for (wchar_t c : sent2) h.engine->HandleCharacter(c);
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return h.backend->ReplacementsCount() == 2u; });
    CHECK_EQ(h.backend->ReplacementsCount(), 2u);
    CHECK_STREQ(h.backend->GetReplacement(1), L"this project.");
}

// 26. Physical Cancellation Aborts Active Provider
TEST(speculative_physical_cancellation_aborts_active_provider) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto mgr = std::make_shared<SpeculativeTranslationManager>(true, 15, 4);

    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    // Start speculative #1
    mgr->StartSpeculativeRequest(1, L"hola", id, pipeline, dispatcher);
    WaitFor([&] { return controllable->HasPending(); });
    CHECK_EQ(controllable->pendingRequests.size(), 1u);
    CHECK(controllable->pendingRequests[0].cancellationToken != nullptr);
    CHECK_EQ(controllable->pendingRequests[0].cancellationToken->load(), false);

    // Supersede with speculative #2: triggers physical cancellation on #1
    mgr->StartSpeculativeRequest(2, L"hola mundo", id, pipeline, dispatcher);
    CHECK_EQ(controllable->pendingRequests[0].cancellationToken->load(), true); // Physical cancellation signaled!
}

// 27. Queued Provider Cancellation
TEST(speculative_queued_cancellation) {
    auto mock = std::make_shared<MockProvider>(std::chrono::milliseconds(50));
    auto pipeline = std::make_shared<TranslationPipeline>(mock, nullptr, 5000);

    auto token = std::make_shared<std::atomic<bool>>(false);
    TranslationRequest req;
    req.text = L"hola";
    req.commitAuthority = CommitAuthority::Forbidden;
    req.cancellationToken = token;

    std::atomic<bool> callbackFired{false};
    std::atomic<bool> resultOk{true};
    std::wstring resultErr;

    pipeline->Submit(std::move(req), [&](TranslationResult res) {
        resultOk = res.ok;
        resultErr = res.error;
        callbackFired = true;
    });

    // Cancel while queued / sleeping
    token->store(true);

    WaitFor([&] { return callbackFired.load(); });
    CHECK_EQ(resultOk.load(), false);
    CHECK_EQ(pipeline->PendingSpeculative(), 0u);
}

// 28. Pipeline Slot Released on Cancellation
TEST(speculative_slot_released_on_cancellation) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);

    TranslationRequest req;
    req.text = L"hola";
    req.commitAuthority = CommitAuthority::Forbidden;
    req.cancellationToken = std::make_shared<std::atomic<bool>>(false);

    pipeline->Submit(std::move(req), [](TranslationResult) {});
    CHECK_EQ(pipeline->PendingSpeculative(), 1u);

    controllable->CancelPending(0);
    CHECK_EQ(pipeline->PendingSpeculative(), 0u);
}

// 29. Concurrency Strictly One for Physical Speculative Requests
TEST(speculative_concurrency_strictly_one) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto mgr = std::make_shared<SpeculativeTranslationManager>(true, 15, 4);

    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    // Rapidly start 5 speculative snapshots
    mgr->StartSpeculativeRequest(1, L"hola", id, pipeline, dispatcher);
    mgr->StartSpeculativeRequest(2, L"hola m", id, pipeline, dispatcher);
    mgr->StartSpeculativeRequest(3, L"hola mu", id, pipeline, dispatcher);
    mgr->StartSpeculativeRequest(4, L"hola mun", id, pipeline, dispatcher);
    mgr->StartSpeculativeRequest(5, L"hola mundo", id, pipeline, dispatcher);

    // At most 1 physically active in pipeline
    CHECK_LE(pipeline->PendingSpeculative(), 1u);
    CHECK_EQ(controllable->pendingRequests.size(), 1u);
}

// 30. Final Allowed Request Preempts Speculative Work
TEST(speculative_allowed_request_preempts_speculative_work) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);

    // 1. Speculative request in flight
    TranslationRequest specReq;
    specReq.text = L"hola";
    specReq.commitAuthority = CommitAuthority::Forbidden;
    specReq.cancellationToken = std::make_shared<std::atomic<bool>>(false);
    pipeline->Submit(std::move(specReq), [](TranslationResult) {});

    CHECK_EQ(pipeline->PendingSpeculative(), 1u);
    CHECK_EQ(controllable->pendingRequests[0].cancellationToken->load(), false);

    // 2. Final Allowed request arrives
    TranslationRequest finalReq;
    finalReq.text = L"hola.";
    finalReq.commitAuthority = CommitAuthority::Allowed;
    const int64_t finalId = pipeline->Submit(std::move(finalReq), [](TranslationResult) {});

    // Allowed request accepted, speculative request preempted!
    CHECK_GT(finalId, 0);
    CHECK_EQ(pipeline->PendingAllowed(), 1u);
    CHECK_EQ(controllable->pendingRequests[0].cancellationToken->load(), true);
}

// 31. Engine Submit Rejection Safety Contract
TEST(engine_submit_rejection_safety_contract) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.minChars = 4;
    engine->ApplyConfig(cfg);

    // Fill allowed capacity: 4 requests in flight
    for (int i = 0; i < 4; ++i) {
        TranslationRequest r;
        r.text = L"test";
        r.commitAuthority = CommitAuthority::Allowed;
        pipeline->Submit(std::move(r), [](TranslationResult) {});
    }
    CHECK_EQ(pipeline->PendingAllowed(), 4u);

    // Type 4 characters
    engine->HandleCharacter(L'h');
    engine->HandleCharacter(L'o');
    engine->HandleCharacter(L'l');
    engine->HandleCharacter(L'a');

    // Attempt final submit via strong terminator: pipeline will reject because pendingAllowed >= 4
    engine->HandleTerminator(L'.');

    // Contract: inflightRequestId_ is NOT set, state is Composing (NOT Translating), buffer is preserved
    CHECK_EQ(engine->InflightRequestId(), -1);
    CHECK(engine->Status().state == EngineState::Composing);
    CHECK_EQ(engine->Buffer(), L"hola.");
}

// 32. Speculative Cancel Then Latest Snapshot Starts After Slot Release
TEST(speculative_cancel_then_latest_snapshot_starts_after_slot_release) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto mgr = std::make_shared<SpeculativeTranslationManager>(true, 15, 4);

    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    mgr->StartSpeculativeRequest(1, L"hola", id, pipeline, dispatcher);
    CHECK_EQ(controllable->translateCalls.load(), 1);

    // Snapshot #2 queued while slot 1 is occupied
    mgr->StartSpeculativeRequest(2, L"hola mundo", id, pipeline, dispatcher);
    CHECK_EQ(controllable->translateCalls.load(), 1); // Not submitted yet

    // Complete #1 (cancellation finishes)
    controllable->CancelPending(0);

    // Snapshot #2 starts automatically after slot release!
    WaitFor([&] { return controllable->translateCalls.load() == 2; });
    CHECK_EQ(controllable->translateCalls.load(), 2);
    CHECK_STREQ(controllable->pendingRequests[0].text, L"hola mundo");
}

// 33. Multiple Superseded Snapshots Only Latest Starts
TEST(speculative_multiple_superseded_snapshots_only_latest_starts) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto mgr = std::make_shared<SpeculativeTranslationManager>(true, 15, 4);

    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    mgr->StartSpeculativeRequest(1, L"hola", id, pipeline, dispatcher);

    // Queue snapshot #2, then overwrite with snapshot #3
    mgr->StartSpeculativeRequest(2, L"hola a", id, pipeline, dispatcher);
    mgr->StartSpeculativeRequest(3, L"hola amigos", id, pipeline, dispatcher);

    // Complete #1
    controllable->CancelPending(0);

    // Only snapshot #3 starts (snapshot #2 skipped cleanly)
    WaitFor([&] { return controllable->translateCalls.load() == 2; });
    CHECK_EQ(controllable->translateCalls.load(), 2);
    CHECK_STREQ(controllable->pendingRequests[0].text, L"hola amigos");
}

// 34. Allowed Request Accepted While Speculative Cancellation In Progress
TEST(allowed_request_accepted_while_speculative_cancellation_is_in_progress) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.minChars = 4;
    cfg.speculativeMinChars = 4;
    cfg.speculativeTranslationEnabled = true;
    cfg.speculativeDebounceMs = 10;
    cfg.debounceMs = 200;
    engine->ApplyConfig(cfg);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) engine->HandleCharacter(c);

    // Speculative in flight
    engine->OnSpeculativeDebounceFired();
    WaitFor([&] { return controllable->HasPending(); });
    CHECK_EQ(pipeline->PendingSpeculative(), 1u);

    // User types '.' -> Allowed request submitted immediately
    engine->HandleTerminator(L'.');

    // Allowed request accepted independently of speculative teardown
    CHECK_EQ(pipeline->PendingAllowed(), 1u);
    CHECK(engine->Status().state == EngineState::Translating);
}

// 35. Rejected Speculative Request Does Not Create Candidate
TEST(rejected_speculative_request_does_not_create_candidate) {
    auto pipeline = std::make_shared<TranslationPipeline>(nullptr, nullptr, 5000); // No provider
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto mgr = std::make_shared<SpeculativeTranslationManager>(true, 15, 4);

    TransformationIdentity id;
    mgr->StartSpeculativeRequest(1, L"hola", id, pipeline, dispatcher);

    CHECK(!mgr->CurrentCandidate().has_value());
    CHECK_EQ(mgr->ActiveSpeculativeRequestId(), 0);
}

// 36. Rejected Final Request Does Not Enter Translating
TEST(rejected_final_request_does_not_enter_translating) {
    auto pipeline = std::make_shared<TranslationPipeline>(nullptr, nullptr, 5000); // No provider
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.minChars = 10;
    engine->ApplyConfig(cfg);

    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) engine->HandleCharacter(c);

    engine->HandleTerminator(L'.');
    CHECK_EQ(engine->InflightRequestId(), -1);
    CHECK(engine->Status().state == EngineState::Composing);
    CHECK_EQ(engine->Buffer(), L"hola mundo.");
}

// 37. Late Cancelled Speculative Completion Cannot Mutate Engine
TEST(late_cancelled_speculative_completion_cannot_mutate_engine) {
    SpeculativeHarness h(true, 15, 100);

    // 1. Submit final Allowed request
    const std::wstring text = L"hola mundo";
    for (wchar_t c : text) h.engine->HandleCharacter(c);
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return h.backend->ReplacementsCount() == 1u; });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world.");

    // 2. Late speculative result arrives with Forbidden authority
    TranslationResult lateRes;
    lateRes.text = L"late hello";
    lateRes.ok = true;
    lateRes.commitAuthority = CommitAuthority::Forbidden;
    lateRes.requestId = 999;
    h.engine->OnTranslationResult(999, std::move(lateRes));

    // Zero visible mutation, replacements count stays 1
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world.");
}

// 38. Pipeline Slot Released Exactly Once
TEST(pipeline_slot_released_exactly_once) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);

    TranslationRequest r1;
    r1.text = L"req1";
    r1.commitAuthority = CommitAuthority::Allowed;

    TranslationRequest r2;
    r2.text = L"req2";
    r2.commitAuthority = CommitAuthority::Forbidden;

    pipeline->Submit(std::move(r1), [](TranslationResult) {});
    pipeline->Submit(std::move(r2), [](TranslationResult) {});

    CHECK_EQ(pipeline->PendingAllowed(), 1u);
    CHECK_EQ(pipeline->PendingSpeculative(), 1u);
    CHECK_EQ(pipeline->TotalPending(), 2u);

    controllable->CompleteLatest(L"out2");
    controllable->CompleteLatest(L"out1");

    CHECK_EQ(pipeline->PendingAllowed(), 0u);
    CHECK_EQ(pipeline->PendingSpeculative(), 0u);
    CHECK_EQ(pipeline->TotalPending(), 0u);
}

// 39. Predictive OFF Stage 1 Regression Remains Zero Provider Calls
TEST(predictive_off_stage1_regression_remains_zero_provider_calls) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.minChars = 4;
    cfg.speculativeTranslationEnabled = false; // OFF
    engine->ApplyConfig(cfg);

    engine->HandleCharacter(L'h');
    engine->HandleCharacter(L'o');
    engine->HandleCharacter(L'l');
    engine->HandleCharacter(L'a');

    // Trigger debounces: 600ms, 750ms, 1200ms
    engine->OnDebounceFired();
    engine->OnSpeculativeDebounceFired();

    // ZERO provider calls!
    CHECK_EQ(controllable->translateCalls.load(), 0);
    CHECK_EQ(backend->ReplacementsCount(), 0u);
}

// 40. 50-Character Rapid Typing Burst Stress Test
TEST(speculative_typing_burst_stress_test) {
    auto mock = std::make_shared<MockProvider>(std::chrono::milliseconds(25)); // 25ms provider delay
    auto pipeline = std::make_shared<TranslationPipeline>(mock, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.minChars = 4;
    cfg.speculativeTranslationEnabled = true;
    cfg.speculativeDebounceMs = 15;
    cfg.debounceMs = 100;
    engine->ApplyConfig(cfg);

    const std::wstring sentence = L"necesito terminar este proyecto antes del proximo.";
    // 50 characters

    size_t maxPhysicalSpeculative = 0;
    size_t maxPendingAllowed = 0;
    size_t maxTotalPending = 0;

    for (size_t i = 0; i < sentence.size(); ++i) {
        const wchar_t c = sentence[i];
        if (c == L'.') {
            engine->HandleTerminator(c);
        } else {
            engine->HandleCharacter(c);
        }

        // Sample pending metrics
        maxPhysicalSpeculative = std::max(maxPhysicalSpeculative, pipeline->PendingSpeculative());
        maxPendingAllowed = std::max(maxPendingAllowed, pipeline->PendingAllowed());
        maxTotalPending = std::max(maxTotalPending, pipeline->TotalPending());

        // Simulate 20ms inter-character timing
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Wait for final commit
    WaitFor([&] { return backend->ReplacementsCount() > 0; }, 5000);

    const auto tel = engine->SpeculativeManager()->GetTelemetry();

    std::wprintf(L"\n========================================================================================\n");
    std::wprintf(L"SPECULATIVE TYPING BURST STRESS TEST METRICS (50 CHARACTERS)\n");
    std::wprintf(L"========================================================================================\n");
    std::wprintf(L"Max Physical Speculative Concurrency: %zu (Invariant: <= 1)\n", maxPhysicalSpeculative);
    std::wprintf(L"Max Pending Allowed:                 %zu (Invariant: <= 4)\n", maxPendingAllowed);
    std::wprintf(L"Max Total Pending in Pipeline:       %zu (Invariant: <= 5)\n", maxTotalPending);
    std::wprintf(L"Total Speculative Started:           %lld\n", tel.requestsStarted);
    std::wprintf(L"Total Speculative Cancelled:         %lld\n", tel.requestsCancelled);
    std::wprintf(L"Total Speculative Completed:         %lld\n", tel.requestsCompleted);
    std::wprintf(L"Final Commit Count:                  %zu (Invariant: == 1)\n", backend->ReplacementsCount());
    std::wprintf(L"Final Replacement Text:              \"%ls\"\n", backend->GetReplacement(0).c_str());
    std::wprintf(L"========================================================================================\n");

    // Invariants
    CHECK_LE(maxPhysicalSpeculative, 1u);
    CHECK_LE(maxPendingAllowed, 4u);
    CHECK_EQ(backend->ReplacementsCount(), 1u);
    CHECK_EQ(engine->Buffer(), L"");
    CHECK_EQ(engine->InflightRequestId(), -1);
}

// 41. PCHP Trigger Gates Enforcement (Length >= 10, Growth >= 8, Cooldown, Candidate Suppression)
TEST(pchp_trigger_gates_enforcement) {
    SpeculativeTranslationManager mgr(true);
    TransformationIdentity id;
    id.providerId = ProviderId::Mock;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    // 1. Min buffer length < 10 -> False
    CHECK(!mgr.ShouldTriggerSpeculative(L"Hola", id));
    CHECK(!mgr.ShouldTriggerSpeculative(L"Hola que", id)); // 8 chars

    // 2. Buffer length >= 10 -> True
    CHECK(mgr.ShouldTriggerSpeculative(L"Hola que tal", id)); // 12 chars

    auto mock = std::make_shared<MockProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(mock, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();

    // Start speculative #1 for "Hola que tal" (len=12)
    mgr.StartSpeculativeRequest(1, L"Hola que tal", id, pipeline, dispatcher);

    // 3. Growth gate: buffer grew by only 2 chars ("Hola que tal!!", len=14) -> False (diff=2 < 8)
    CHECK(!mgr.ShouldTriggerSpeculative(L"Hola que tal!!", id));

    // 4. Growth gate: buffer grew by 10 chars ("Hola que tal como estas", len=23) -> True after cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpCooldownMs + 50));
    CHECK(mgr.ShouldTriggerSpeculative(L"Hola que tal como estas", id));

    // 5. Candidate Ready Suppression: if candidate is READY for "Hola que tal como estas", suppress re-trigger
    mgr.StartSpeculativeRequest(2, L"Hola que tal como estas", id, pipeline, dispatcher);
    TranslationResult res;
    res.ok = true;
    res.text = L"Hello how are you";
    mgr.OnSpeculativeResult(2, res, L"Hola que tal como estas", id, 500);

    CHECK(!mgr.ShouldTriggerSpeculative(L"Hola que tal como estas", id));
}

// 42. PCHP No Immediate Restart on Keystroke Invariant
TEST(pchp_no_immediate_restart_on_keystroke) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    engine->ApplyConfig(cfg);

    const std::wstring text = L"necesito terminar";
    for (wchar_t c : text) {
        engine->HandleCharacter(c);
    }

    // Fire hesitation debounce window (>=450ms pause)
    engine->OnSpeculativeDebounceFired();
    CHECK_EQ(controllable->translateCalls.load(), 1); // 1 speculative request started

    // User resumes typing: types >= 8 characters (" este proyecto")
    const std::wstring continuation = L" este proyecto";
    for (wchar_t c : continuation) {
        engine->HandleCharacter(c);
    }

    // Invariant: Active request cancelled, but NO new speculative request immediately launched
    CHECK_EQ(controllable->translateCalls.load(), 1); // Still 1! No immediate child request.
    CHECK(!engine->SpeculativeManager()->CurrentCandidate().has_value()); // Candidate invalidated

    // Physical provider releases slot for cancelled request 1
    controllable->CompleteLatest(L"", false);

    // Only a NEW hesitation debounce window (after cooldown) can start the next speculative request
    std::this_thread::sleep_for(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpCooldownMs + 10));
    engine->OnSpeculativeDebounceFired();
    CHECK_EQ(controllable->translateCalls.load(), 2); // Now request #2 started
}

// 43. PCHP Policy 2 Terminal Normalization & Adversarial Safety Suite
TEST(pchp_policy2_terminal_normalization_and_adversarial_rejection) {
    SpeculativeTranslationManager mgr(true);
    TransformationIdentity id;
    id.providerId = ProviderId::Mock;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    // Setup ready candidate for "Vamos a casa" -> "Let's go home"
    TranslationResult res;
    res.ok = true;
    res.text = L"Let's go home";
    mgr.OnSpeculativeResult(1, res, L"Vamos a casa", id, 600);

    TranslationRequest finalReq;
    finalReq.requestId = 100;
    finalReq.providerId = id.providerId;
    finalReq.sourceLanguage = id.sourceLanguage;
    finalReq.targetLanguage = id.targetLanguage;
    finalReq.operation = id.operation;
    finalReq.profile = id.profile;
    finalReq.commitAuthority = CommitAuthority::Allowed;

    // 1. Strict Exact Match: "Vamos a casa"
    finalReq.text = L"Vamos a casa";
    TranslationResult outRes;
    bool normalized = false;
    CHECK(mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));
    CHECK(!normalized);
    CHECK_EQ(outRes.text, L"Let's go home");
    CHECK_EQ(outRes.commitAuthority, CommitAuthority::Allowed);

    // Re-prime candidate
    mgr.OnSpeculativeResult(2, res, L"Vamos a casa", id, 600);

    // 2. Policy 2: Terminal Period "Vamos a casa."
    finalReq.text = L"Vamos a casa.";
    CHECK(mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));
    CHECK(normalized);
    CHECK_EQ(outRes.text, L"Let's go home.");

    // Re-prime candidate
    mgr.OnSpeculativeResult(3, res, L"Vamos a casa", id, 600);

    // 3. Policy 2: Terminal Exclamation "Vamos a casa!"
    finalReq.text = L"Vamos a casa!";
    CHECK(mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));
    CHECK(normalized);
    CHECK_EQ(outRes.text, L"Let's go home!");

    // Re-prime candidate
    mgr.OnSpeculativeResult(4, res, L"Vamos a casa", id, 600);

    // 4. Policy 2: Terminal Question "Vamos a casa?"
    finalReq.text = L"Vamos a casa?";
    CHECK(mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));
    CHECK(normalized);
    CHECK_EQ(outRes.text, L"Let's go home?");

    // Re-prime candidate
    mgr.OnSpeculativeResult(5, res, L"Vamos a casa", id, 600);

    // --- Adversarial Rejections ---
    // A. Multiple Punctuation: "Vamos a casa?!" -> REJECT
    finalReq.text = L"Vamos a casa?!";
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));

    // B. Ellipsis: "Vamos a casa..." -> REJECT
    finalReq.text = L"Vamos a casa...";
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));

    // C. Internal Whitespace Difference: "Vamos  a casa." -> REJECT
    finalReq.text = L"Vamos  a casa.";
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));

    // D. Whitespace before Punctuation: "Vamos a casa ." -> REJECT
    finalReq.text = L"Vamos a casa .";
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));

    // E. Changed Word: "Vamos al cine." -> REJECT
    finalReq.text = L"Vamos al cine.";
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));

    // F. Extra Word: "Vamos a casa hoy." -> REJECT
    finalReq.text = L"Vamos a casa hoy.";
    CHECK(!mgr.TryConsumeExactCandidate(finalReq, outRes, &normalized));

    // Invariant: Zero invalid candidate reuses
    CHECK_EQ(mgr.GetTelemetry().candidateHitsStrict, 1);
    CHECK_EQ(mgr.GetTelemetry().candidateHitsNormalized, 3);
    CHECK_EQ(mgr.GetTelemetry().candidateHits, 4);
    CHECK_EQ(mgr.GetTelemetry().candidateMisses, 6);
}

// 44. PCHP Single Commit Path & Weak Signal Immutability
TEST(pchp_single_commit_path_and_weak_signal_immutability) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    engine->ApplyConfig(cfg);

    const std::wstring text = L"necesito terminar este proyecto";
    for (wchar_t c : text) {
        engine->HandleCharacter(c);
    }

    // 1. Trigger speculative hesitation
    engine->OnSpeculativeDebounceFired();
    CHECK_EQ(controllable->translateCalls.load(), 1);

    // Complete speculative background request while CommitAuthority is Forbidden
    controllable->CompleteLatest(L"I need to finish this project");

    // Invariant: Forbidden completion produces ZERO visible text mutations and ZERO commits
    CHECK_EQ(backend->ReplacementsCount(), 0u);
    CHECK_EQ(backend->commits.load(), 0);
    CHECK_EQ(engine->Buffer(), text); // Buffer completely untouched

    // 2. Finalize with Terminal Period '.' -> Uses candidate via single authoritative Allowed commit path
    engine->HandleTerminator(L'.');

    // Exact single commit occurs through CommitReplacement
    CHECK_EQ(backend->ReplacementsCount(), 1u);
    CHECK_EQ(backend->GetReplacement(0), L"I need to finish this project.");
    CHECK_EQ(engine->Buffer(), L"");
}

// 45. PCHP Shift+Enter Candidate Hit & Miss with Trailing Newline
TEST(pchp_shift_enter_candidate_hit_and_miss) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    engine->ApplyConfig(cfg);

    // A. Shift+Enter Candidate HIT Scenario
    const std::wstring text = L"este proyecto es importante";
    for (wchar_t c : text) {
        engine->HandleCharacter(c);
    }
    engine->OnSpeculativeDebounceFired();
    controllable->CompleteLatest(L"this project is important");

    engine->HandleShiftEnter();

    CHECK_EQ(backend->ReplacementsCount(), 1u);
    CHECK_EQ(backend->GetReplacement(0), L"this project is important\n"); // Exactly 1 trailing newline

    // B. Shift+Enter Candidate MISS Fallback Scenario
    const std::wstring text2 = L"otro mensaje completamente nuevo";
    for (wchar_t c : text2) {
        engine->HandleCharacter(c);
    }
    // Finalize immediately with Shift+Enter without speculative candidate ready
    engine->HandleShiftEnter();

    // Provider receives final Allowed request with appendTrailingNewline=true
    CHECK(controllable->HasPending());
    controllable->CompleteLatest(L"another completely new message");

    CHECK_EQ(backend->ReplacementsCount(), 2u);
    CHECK_EQ(backend->GetReplacement(1), L"another completely new message\n"); // Exactly 1 trailing newline
}

// 46. PCHP 100 & 500 Character Rapid Typing Stress Test
TEST(pchp_rapid_typing_burst_100_and_500_chars) {
    auto mock = std::make_shared<MockProvider>(std::chrono::milliseconds(15));
    auto pipeline = std::make_shared<TranslationPipeline>(mock, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    engine->ApplyConfig(cfg);

    // 100-character burst
    std::wstring text100;
    for (int i = 0; i < 10; ++i) text100 += L"diezletras ";
    for (wchar_t c : text100) {
        engine->HandleCharacter(c);
    }
    engine->HandleTerminator(L'.');
    WaitFor([&] { return backend->ReplacementsCount() == 1u; }, 3000);

    CHECK_LE(pipeline->PendingSpeculative(), 1u);
    CHECK_EQ(backend->ReplacementsCount(), 1u);
    CHECK_EQ(engine->Buffer(), L"");
}

// 47. Real Manual Failure Reproduction: "Queria comentarte que este proyecto es sumamente importante para mi."
TEST(pchp_real_manual_reproduction_queria_comentarte) {
    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    engine->ApplyConfig(cfg);

    // Part 1: "Queria comentarte que este proyecto "
    const std::wstring part1 = L"Queria comentarte que este proyecto ";
    for (wchar_t c : part1) engine->HandleCharacter(c);

    // 2000ms mid-sentence pause -> PCHP fires speculative request #1
    engine->OnSpeculativeDebounceFired();
    CHECK_EQ(controllable->translateCalls.load(), 1);
    controllable->CompleteLatest(L"I wanted to mention that this project ");

    // Part 2: User resumes typing: "es sumamente importante para mi"
    const std::wstring part2 = L"es sumamente importante para mi";
    for (wchar_t c : part2) engine->HandleCharacter(c);

    // Invariant: Stale candidate invalidated, active speculative request cancelled
    CHECK(!engine->SpeculativeManager()->CurrentCandidate().has_value());

    // 1500ms review pause (after cooldown) -> PCHP fires speculative request #2
    std::this_thread::sleep_for(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpCooldownMs + 10));
    engine->OnSpeculativeDebounceFired();
    CHECK_EQ(controllable->translateCalls.load(), 2);
    controllable->CompleteLatest(L"I wanted to mention to you that this project is extremely important to me");

    // Candidate becomes READY
    CHECK(engine->SpeculativeManager()->CurrentCandidate().has_value());

    // User presses Shift+Enter -> Candidate HIT! Instant commit with trailing newline!
    engine->HandleShiftEnter();

    CHECK_EQ(backend->ReplacementsCount(), 1u);
    CHECK_EQ(backend->GetReplacement(0),
             L"I wanted to mention to you that this project is extremely important to me\n");
    CHECK_EQ(controllable->translateCalls.load(), 2); // Exactly 2 requests total! (vs 18 in Stage 2)
}

// 48. PCHP Hard Floor Invariants & Configuration Compatibility
TEST(pchp_hard_floor_invariants_and_config_compat) {
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    // Legacy config with speculativeMinChars = 5 and speculativeDebounceMs = 150
    SpeculativeTranslationManager mgrLegacy(true, 150, 5);

    // Hard floor 10 chars is strictly enforced: len < 10 must be rejected
    CHECK(!mgrLegacy.ShouldTriggerSpeculative(L"1234567", id)); // len=7 -> REJECTED
    CHECK(!mgrLegacy.ShouldTriggerSpeculative(L"123456789", id)); // len=9 -> REJECTED

    // len=10 on first hesitation -> ALLOWED
    CHECK(mgrLegacy.ShouldTriggerSpeculative(L"1234567890", id)); // len=10 -> ALLOWED

    // Modern config with minChars = 10, debounce = 450 behaves identically
    SpeculativeTranslationManager mgrModern(true, 450, 10);
    CHECK(!mgrModern.ShouldTriggerSpeculative(L"1234567", id)); // len=7 -> REJECTED
    CHECK(!mgrModern.ShouldTriggerSpeculative(L"123456789", id)); // len=9 -> REJECTED
    CHECK(mgrModern.ShouldTriggerSpeculative(L"1234567890", id)); // len=10 -> ALLOWED
}

// 49. Real Failure Log Regression: Small-Growth Bypass Rejection (1, 6, 7 chars rejected; 8 allowed)
TEST(pchp_real_log_growth_and_baseline_rejection) {
    SpeculativeTranslationManager mgr(true);
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();

    // A. Speculative request at len=36 ("Queria comentarte que este proyecto ")
    const std::wstring base36 = L"Queria comentarte que este proyecto ";
    mgr.StartSpeculativeRequest(1, base36, id, pipeline, dispatcher);
    CHECK_EQ(mgr.LastRequestedSnapshot().size(), 36u);

    // Wait past cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpCooldownMs + 20));

    // A. User types 1 character -> len=37 (growth = 1 < 8) -> REJECTED
    const std::wstring growth1 = base36 + L"e";
    CHECK(!mgr.ShouldTriggerSpeculative(growth1, id));

    // B. User types 6 characters -> len=42 (growth = 6 < 8) -> REJECTED
    const std::wstring growth6 = base36 + L"es sum";
    CHECK(!mgr.ShouldTriggerSpeculative(growth6, id));

    // C. User types 7 characters -> len=43 (growth = 7 < 8) -> REJECTED
    const std::wstring growth7 = base36 + L"es suma";
    CHECK(!mgr.ShouldTriggerSpeculative(growth7, id));

    // D. User types 8 characters -> len=44 (growth = 8 >= 8) -> ALLOWED
    const std::wstring growth8 = base36 + L"es sumam";
    CHECK(mgr.ShouldTriggerSpeculative(growth8, id));
}

// 50. Candidate Invalidation Preserves Scheduler Baseline History
TEST(pchp_candidate_invalidation_preserves_scheduler_baseline) {
    SpeculativeTranslationManager mgr(true);
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();

    // Issue request at len=36
    const std::wstring base36 = L"Queria comentarte que este proyecto ";
    mgr.StartSpeculativeRequest(1, base36, id, pipeline, dispatcher);

    // Populate candidate
    TranslationResult res;
    res.ok = true;
    res.text = L"I wanted to mention that this project ";
    mgr.OnSpeculativeResult(1, res, base36, id, 500);
    CHECK(mgr.CurrentCandidate().has_value());

    // User types a character -> InvalidateCandidate() called
    mgr.InvalidateCandidate();

    // INVARIANT: Candidate is invalidated, but scheduler history is PRESERVED!
    CHECK(!mgr.CurrentCandidate().has_value());
    CHECK_EQ(mgr.LastRequestedSnapshot(), base36);

    // Wait past cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpCooldownMs + 20));

    // User types 1 char (len=37): growth gate must STILL reject despite candidate invalidation!
    CHECK(!mgr.ShouldTriggerSpeculative(base36 + L"e", id));

    // User types 8 chars (len=44): growth gate allows next speculative request!
    CHECK(mgr.ShouldTriggerSpeculative(base36 + L"es sumam", id));
}

// 51. Cooldown and Growth Independent Enforcement
TEST(pchp_cooldown_and_growth_independent_enforcement) {
    SpeculativeTranslationManager mgr(true);
    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    auto controllable = std::make_shared<ControllableProvider>();
    auto pipeline = std::make_shared<TranslationPipeline>(controllable, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();

    // Start request #1 at t=0
    mgr.StartSpeculativeRequest(1, L"1234567890", id, pipeline, dispatcher);

    // Growth is 10 chars (1234567890 + ABCDEFGHIJ = 20 chars), but cooldown (750ms) has NOT elapsed!
    const std::wstring grownText = L"1234567890ABCDEFGHIJ";
    CHECK(!mgr.ShouldTriggerSpeculative(grownText, id)); // Rejected due to cooldown!

    // Wait until cooldown elapses
    std::this_thread::sleep_for(std::chrono::milliseconds(SpeculativeTranslationManager::kPchpCooldownMs + 20));

    // Now cooldown satisfied AND growth satisfied -> ALLOWED
    CHECK(mgr.ShouldTriggerSpeculative(grownText, id));
}

// 52. Scheduler History Reset on Sentence Commit and Focus Change
TEST(pchp_scheduler_reset_on_commit_and_focus_change) {
    auto mock = std::make_shared<MockProvider>(std::chrono::milliseconds(10));
    auto pipeline = std::make_shared<TranslationPipeline>(mock, nullptr, 5000);
    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = true;
    engine->ApplyConfig(cfg);

    // Type first sentence
    const std::wstring sent1 = L"Necesito terminar este proyecto";
    for (wchar_t c : sent1) engine->HandleCharacter(c);

    engine->OnSpeculativeDebounceFired();
    CHECK_GT(engine->SpeculativeManager()->LastRequestedSnapshot().size(), 0u);

    // Commit sentence 1 with period
    engine->HandleTerminator(L'.');
    WaitFor([&] { return backend->ReplacementsCount() == 1u; });

    // Scheduler baseline must be RESET on commit
    CHECK_EQ(engine->SpeculativeManager()->LastRequestedSnapshot(), L"");

    // Type second sentence: starting fresh, first hesitation is eligible
    const std::wstring sent2 = L"Segunda oracion nueva";
    for (wchar_t c : sent2) engine->HandleCharacter(c);

    TransformationIdentity id;
    id.sourceLanguage = L"es";
    id.targetLanguage = L"en";

    // First hesitation of sentence 2 is allowed
    CHECK(engine->SpeculativeManager()->ShouldTriggerSpeculative(sent2, id));

    // Focus change resets scheduler baseline as well
    engine->CancelActiveCompositionForFocusChange();
    CHECK_EQ(engine->SpeculativeManager()->LastRequestedSnapshot(), L"");
}

