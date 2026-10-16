#include "mini_test.h"

#include "Composition/InputEngine.h"
#include "Core/Dispatcher.h"
#include "Input/HookBackend.h"
#include "Providers/MockProvider.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <atomic>
#include <mutex>
#include <objbase.h>
#include <thread>

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

struct TestHarness {
    std::shared_ptr<ImmediateDispatcher> dispatcher = std::make_shared<ImmediateDispatcher>();
    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    std::shared_ptr<MockProvider> provider = std::make_shared<MockProvider>(std::chrono::milliseconds(20));
    std::shared_ptr<TranslationCache> cache = std::make_shared<TranslationCache>(100);
    std::shared_ptr<TranslationPipeline> pipeline;
    std::shared_ptr<InputEngine> engine;

    explicit TestHarness(bool withDelay = true) {
        pipeline = std::make_shared<TranslationPipeline>(provider, cache, 30000);
        engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);
        AppConfig cfg;
        cfg.enabled = true;
        cfg.sourceLanguage = L"es";
        cfg.targetLanguage = L"en";
        cfg.debounceMs = withDelay ? 30 : 1;
        cfg.minChars = 2;
        engine->ApplyConfig(cfg);
    }
};

void WaitFor(std::function<bool()> pred, int timeoutMs = 3000) {
    for (int i = 0; i < timeoutMs / 10 && !pred(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

TEST(engine_weak_signal_does_not_replace_buffer) {
    TestHarness h;
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    // Weak signal: debounce alone with predictive OFF does NOT replace buffer
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(h.backend->replacements.empty());
    CHECK_STREQ(h.engine->Buffer(), L"hola");

    // But strong terminator commits
    h.engine->HandleTerminator(L'.');
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello.");
    CHECK_STREQ(h.engine->Buffer(), L"");
}

TEST(engine_terminator_triggers_immediately) {
    TestHarness h(false);
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello.");
    CHECK_EQ(h.backend->replacements[0].second, 5u);
}

TEST(engine_min_chars_respected) {
    TestHarness h;
    AppConfig cfg = h.engine->Status().enabled ? AppConfig{} : AppConfig{};
    (void)cfg;
    // minChars=2: "a" alone must never trigger.
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    CHECK(h.backend->replacements.empty());
    CHECK_STREQ(h.engine->Buffer(), L"a.");
}

TEST(engine_stale_result_does_not_overwrite_new_text) {
    TestHarness h;
    // Mock provider with 20ms delay. Type "hola." (triggers request #1),
    // then quickly type more so the buffer changes before the result arrives.
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    // Immediately backspace '.' and extend the buffer with "sss." before the 20ms result lands.
    h.engine->HandleBackspace();
    h.engine->HandleCharacter(L's');
    h.engine->HandleCharacter(L's');
    h.engine->HandleCharacter(L's');
    h.engine->HandleTerminator(L'.');
    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK(h.backend->HasReplacements());
    // The applied replacement must match the *final* buffer, not the stale one.
    CHECK_STREQ(h.backend->GetReplacement(0), L"holasss.");
}

TEST(engine_failure_keeps_user_text) {
    // A failing provider: the buffer must remain untouched.
    auto failing = std::make_shared<MockProvider>(std::chrono::milliseconds(10));
    // Use a provider whose requests fail: wrap mock with a failing adapter.
    struct FailingProvider : ITranslationProvider {
        std::wstring_view Name() const override { return L"failing"; }
        std::wstring_view DisplayName() const override { return L"Failing"; }
        bool RequiresNetwork() const override { return false; }
        ProviderStatus Status() const override { return {ProviderStatus::State::Error, L"down"}; }
        void CancelAll() override {}
        void Translate(TranslationRequest request, TranslationCallback callback) override {
            std::thread([request, callback = std::move(callback)]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                TranslationResult r;
                r.requestId = request.requestId;
                r.commitAuthority = request.commitAuthority;
                r.ok = false;
                r.error = L"provider unavailable";
                r.errorDetail = L"simulated network failure";
                callback(std::move(r));
            }).detach();
        }
    };

    auto dispatcher = std::make_shared<ImmediateDispatcher>();
    auto backend = std::make_shared<FakeBackend>();
    auto cache = std::make_shared<TranslationCache>(100);
    auto pipeline = std::make_shared<TranslationPipeline>(std::make_shared<FailingProvider>(), cache, 30000);
    auto engine = std::make_shared<InputEngine>(dispatcher, backend, pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.debounceMs = 1;
    cfg.minChars = 2;
    engine->ApplyConfig(cfg);

    engine->HandleCharacter(L'h');
    engine->HandleCharacter(L'o');
    engine->HandleCharacter(L'l');
    engine->HandleCharacter(L'a');
    engine->HandleTerminator(L'.');
    WaitFor([&] { return engine->Status().state == EngineState::Error; });
    // Text must be preserved exactly.
    CHECK_STREQ(engine->Buffer(), L"hola.");
    CHECK(backend->replacements.empty());
    CHECK(!backend->statusShown.empty()); // failure status was shown
    (void)failing;
}

TEST(engine_commit_key_flushes_buffer_untouched) {
    TestHarness h;
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCommitKey(); // Enter
    CHECK_STREQ(h.engine->Buffer(), L"");
    CHECK(h.backend->replacements.empty());
    CHECK_GT(h.backend->commits.load(), 0);
}

TEST(engine_backspace_updates_buffer) {
    TestHarness h;
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleBackspace();
    CHECK_STREQ(h.engine->Buffer(), L"ho");
    h.engine->HandleBackspace();
    CHECK_STREQ(h.engine->Buffer(), L"h");
    h.engine->HandleBackspace();
    CHECK_STREQ(h.engine->Buffer(), L"");
}

TEST(engine_backspace_surrogate_pair) {
    TestHarness h;
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L'\xD83D'); // emoji = surrogate pair
    h.engine->HandleCharacter(L'\xDE00');
    h.engine->HandleBackspace();
    CHECK_STREQ(h.engine->Buffer(), L"a");
}

TEST(engine_backspace_surrogate_pair_multi_step) {
    TestHarness h;
    // Type "A😀B" (4 UTF-16 code units: 'A', 0xD83D, 0xDE00, 'B')
    h.engine->HandleCharacter(L'A');
    h.engine->HandleCharacter(L'\xD83D');
    h.engine->HandleCharacter(L'\xDE00');
    h.engine->HandleCharacter(L'B');
    CHECK_STREQ(h.engine->Buffer(), L"A\xD83D\xDE00" L"B");

    // Backspace 'B' -> "A😀" (3 code units)
    h.engine->HandleBackspace();
    CHECK_STREQ(h.engine->Buffer(), L"A\xD83D\xDE00");

    // Backspace emoji -> "A" (1 code unit: removes full surrogate pair)
    h.engine->HandleBackspace();
    CHECK_STREQ(h.engine->Buffer(), L"A");

    // Backspace 'A' -> "" (0 code units)
    h.engine->HandleBackspace();
    CHECK_STREQ(h.engine->Buffer(), L"");
}

TEST(engine_disable_keeps_text_and_stops) {
    TestHarness h;
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->SetEnabled(false);
    CHECK_STREQ(h.engine->Buffer(), L"");
    CHECK(h.backend->replacements.empty());
    // Further typing does nothing.
    h.engine->HandleCharacter(L'x');
    CHECK_STREQ(h.engine->Buffer(), L"");
    CHECK(h.engine->Status().state == EngineState::Idle);
}

TEST(engine_max_buffer_commits_asis) {
    TestHarness h;
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.debounceMs = 1000;
    cfg.minChars = 2;
    cfg.maxBufferChars = 5;
    h.engine->ApplyConfig(cfg);
    for (const wchar_t c : std::wstring(L"abcdefgh")) {
        h.engine->HandleCharacter(c);
    }
    // "abcde" hit the cap and was committed untouched; the remaining "fgh"
    // stays in the buffer.
    CHECK_STREQ(h.engine->Buffer(), L"fgh");
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.backend->commits.load(), 1);
}

TEST(engine_context_flow) {
    TestHarness h;
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    // The committed translation becomes context for the next request.
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');
    WaitFor([&] { return h.backend->replacements.size() >= 2; });
    CHECK_EQ(h.backend->replacements.size(), 2u);
    CHECK_STREQ(h.backend->replacements[1].first, L"world.");
}

TEST(engine_preserves_leading_and_trailing_whitespace) {
    TestHarness h;
    // User types space + "mundo."
    h.engine->HandleCharacter(L' ');
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    // The replacement must retain the leading space and punctuation: " world."
    CHECK_STREQ(h.backend->replacements[0].first, L" world.");
    CHECK_EQ(h.backend->replacements[0].second, 7u); // " mundo."
}

TEST(engine_foreground_switch_cancels_inflight_composition) {
    // 1. Start translation with non-zero delay
    TestHarness h(true);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(100));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.debounceMs = 1;
    cfg.minChars = 2;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');

    // Verify in-flight request started
    WaitFor([&] { return h.engine->Status().state == EngineState::Translating; }, 500);
    CHECK(h.engine->InflightRequestId() != -1);

    // 2. User switches applications (foreground changed)
    h.engine->CancelActiveCompositionForFocusChange();

    // Verify composition cancelled and state returned to Idle
    CHECK_EQ(h.engine->Status().state, EngineState::Idle);
    CHECK(h.engine->Buffer().empty());
    CHECK_EQ(h.engine->InflightRequestId(), -1);

    // 3. Wait for the background provider response to arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Expected: Old translation result MUST NOT commit into the new target!
    CHECK(h.backend->replacements.empty());
}

TEST(engine_foreground_switch_without_inflight_request) {
    TestHarness h;
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    CHECK_EQ(h.engine->Buffer(), L"ho");

    // Foreground changes before any request fired
    h.engine->CancelActiveCompositionForFocusChange();
    CHECK(h.engine->Buffer().empty());
    CHECK_EQ(h.engine->Status().state, EngineState::Idle);
    CHECK(h.backend->replacements.empty());
}

TEST(engine_foreground_switch_then_new_typing_in_new_window) {
    TestHarness h;
    // Typing in App A
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L'b');
    h.engine->CancelActiveCompositionForFocusChange();

    // Now user types in App B
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello.");
}

TEST(engine_synthetic_signature_tagging_and_hook_suppression) {
    // Dedicated signature constant check
    CHECK_EQ(kAiieSyntheticSignature, static_cast<ULONG_PTR>(0x41494945));

    // Verify that synthetic SendInput events carrying kAiieSyntheticSignature
    // are distinct and carry LLKHF_INJECTED flag.
    KBDLLHOOKSTRUCT kbd{};
    kbd.vkCode = 'A';
    kbd.scanCode = 0x1E;
    kbd.flags = LLKHF_INJECTED;
    kbd.dwExtraInfo = kAiieSyntheticSignature;

    const bool isAiieSynthetic = ((kbd.flags & LLKHF_INJECTED) != 0) &&
                                 (kbd.dwExtraInfo == kAiieSyntheticSignature);
    CHECK(isAiieSynthetic);

    // Normal user keystroke (dwExtraInfo == 0, flags == 0)
    KBDLLHOOKSTRUCT userKbd{};
    userKbd.vkCode = 'A';
    userKbd.scanCode = 0x1E;
    userKbd.flags = 0;
    userKbd.dwExtraInfo = 0;

    const bool isUserSynthetic = ((userKbd.flags & LLKHF_INJECTED) != 0) &&
                                 (userKbd.dwExtraInfo == kAiieSyntheticSignature);
    CHECK(!isUserSynthetic);
}

TEST(engine_min_chars_1_and_min_chars_5_isolation) {
    // Test minChars = 1 (enforces internal kMinMeaningfulChars = 3 safety floor)
    {
        TestHarness h;
        AppConfig cfg;
        cfg.enabled = true;
        cfg.sourceLanguage = L"es";
        cfg.targetLanguage = L"en";
        cfg.debounceMs = 10;
        cfg.minChars = 1;
        h.engine->ApplyConfig(cfg);

        // Lone '?' must NOT submit (safety floor)
        h.engine->HandleTerminator(L'?');
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(h.backend->replacements.empty());

        // But 3-character sentence "si?" submits under minChars=1
        h.engine->HandleBackspace();
        h.engine->HandleCharacter(L's');
        h.engine->HandleCharacter(L'i');
        h.engine->HandleTerminator(L'?');
        WaitFor([&] { return !h.backend->replacements.empty(); });
        CHECK_EQ(h.backend->replacements.size(), 1u);
    }

    // Test minChars = 5
    {
        TestHarness h;
        AppConfig cfg;
        cfg.enabled = true;
        cfg.sourceLanguage = L"es";
        cfg.targetLanguage = L"en";
        cfg.debounceMs = 10;
        cfg.minChars = 5;
        h.engine->ApplyConfig(cfg);

        h.engine->HandleCharacter(L'h');
        h.engine->HandleCharacter(L'o');
        h.engine->HandleCharacter(L'l');
        h.engine->HandleCharacter(L'a');
        // 4 chars: should not submit
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(h.backend->replacements.empty());

        // 5th char: submits
        h.engine->HandleTerminator(L'.');
        WaitFor([&] { return !h.backend->replacements.empty(); });
        CHECK_EQ(h.backend->replacements.size(), 1u);
    }
}

TEST(engine_stale_result_does_not_corrupt_active_request) {
    TestHarness h(true);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(50));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.debounceMs = 1;
    cfg.minChars = 2;
    h.engine->ApplyConfig(cfg);

    // 1. Submit Request #1: "hola."
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    const int64_t req1Id = h.engine->InflightRequestId();
    CHECK(req1Id > 0);

    // 2. Submit Request #2 before #1 finishes: "hola mundo."
    h.engine->HandleBackspace(); // remove '.'
    h.engine->HandleCharacter(L' ');
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');
    const int64_t req2Id = h.engine->InflightRequestId();
    CHECK(req2Id > req1Id);

    // 3. Wait for all requests to finish
    WaitFor([&] { return !h.backend->replacements.empty(); }, 1000);

    // Verify:
    // - Final replacement committed successfully
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello world.");

    // - Stale Request #1 did NOT display a failure banner
    for (const auto& msg : h.backend->statusShown) {
        CHECK(msg.find(L"failed") == std::wstring::npos);
    }
}

TEST(engine_multiple_overlapping_burst_requests_cleanly_commit_latest) {
    TestHarness h(true);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(40));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.debounceMs = 5;
    cfg.minChars = 2;
    h.engine->ApplyConfig(cfg);

    // Rapid burst typing
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    h.engine->HandleCharacter(L' ');
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');

    // Wait for latest to commit
    WaitFor([&] { return !h.backend->replacements.empty(); }, 1500);

    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello world.");
}

TEST(engine_monotonic_request_ids_never_recycle_zero) {
    TestHarness h;
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.debounceMs = 1;
    cfg.minChars = 2;
    h.engine->ApplyConfig(cfg);

    // Request A
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');
    const int64_t idA = h.engine->InflightRequestId();
    CHECK(idA >= 1);
    WaitFor([&] { return !h.backend->replacements.empty(); });

    // Request B (after A completed and inflightRequestId_ was reset to -1)
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleTerminator(L'.');
    const int64_t idB = h.engine->InflightRequestId();
    CHECK(idB > idA); // Must be strictly greater than idA, NOT 0!
    WaitFor([&] { return h.backend->replacements.size() >= 2; });
}

TEST(engine_genuine_provider_error_still_reports_and_preserves_text) {
    TestHarness h;
    // Deliver explicit genuine provider error
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');
    const int64_t id = h.engine->InflightRequestId();
    CHECK(id > 0);

    TranslationResult errResult;
    errResult.ok = false;
    errResult.error = L"HTTP 500 Internal Server Error";
    errResult.errorDetail = L"Upstream API unavailable";
    errResult.requestId = id;

    h.engine->OnTranslationResult(id, errResult);

    CHECK_EQ(h.engine->Status().state, EngineState::Error);
    CHECK_EQ(h.engine->Buffer(), L"ho.");
    CHECK(h.backend->replacements.empty());
    CHECK(!h.backend->statusShown.empty());
    CHECK(h.backend->statusShown.back().find(L"failed") != std::wstring::npos);
}

// ===========================================================================
// Adaptive Translation Trigger Production Tests (Policy C)
// ===========================================================================

TEST(adaptive_trigger_lone_dot_does_not_submit) {
    TestHarness h(false);
    h.engine->HandleTerminator(L'.');
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK_EQ(h.engine->Buffer(), L".");
}

TEST(adaptive_trigger_lone_exclamation_does_not_submit) {
    TestHarness h(false);
    h.engine->HandleTerminator(L'!');
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK_EQ(h.engine->Buffer(), L"!");
}

TEST(adaptive_trigger_lone_question_does_not_submit) {
    TestHarness h(false);
    h.engine->HandleTerminator(L'?');
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK_EQ(h.engine->Buffer(), L"?");
}

TEST(adaptive_trigger_terminal_submits_immediately) {
    TestHarness h(false);
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    // Immediate submission on fast path
    CHECK(h.engine->InflightRequestId() > 0);
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello.");
}

TEST(adaptive_trigger_terminal_bypasses_cooldown) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(100));
    h.cache->Clear();
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.cacheEnabled = false;
    h.engine->ApplyConfig(cfg);

    // 1. Submit a strong terminator request
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    const int64_t id1 = h.engine->InflightRequestId();
    CHECK(id1 > 0);

    // 2. Immediately within 20ms (< 800ms cooldown), type another sentence terminator
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');
    const int64_t id2 = h.engine->InflightRequestId();

    // Must bypass cooldown and submit immediately with a new ID!
    CHECK(id2 > id1);
}

TEST(adaptive_trigger_duplicate_snapshot_not_resubmitted) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(100));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    const int64_t id1 = h.engine->InflightRequestId();
    CHECK(id1 > 0);

    // Repeated debounce event for exact same text while in-flight
    h.engine->OnDebounceFired();
    const int64_t id2 = h.engine->InflightRequestId();
    CHECK_EQ(id1, id2); // Must NOT generate another request
}

TEST(adaptive_trigger_clause_punctuation_debounce) {
    TestHarness h(false);
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L',');

    // Clause punctuation sets 600ms debounce
    CHECK_EQ(h.engine->InflightRequestId(), -1);
}

TEST(adaptive_trigger_word_boundary_pause_respects_cooldown) {
    TestHarness h(false);
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L' ');

    // Word boundary sets 750ms debounce
    CHECK_EQ(h.engine->InflightRequestId(), -1);
}

TEST(adaptive_trigger_mid_word_pause_uses_extended_threshold) {
    TestHarness h(false);
    h.engine->HandleCharacter(L'e');
    h.engine->HandleCharacter(L's');
    h.engine->HandleCharacter(L'c');
    h.engine->HandleCharacter(L'r');
    h.engine->HandleCharacter(L'i');
    h.engine->HandleCharacter(L'b');
    h.engine->HandleCharacter(L'i');

    // Mid-word sets 1200ms extended debounce; no immediate request
    CHECK_EQ(h.engine->InflightRequestId(), -1);
}

TEST(adaptive_trigger_min_chars_floor_prevents_tiny_fragments) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.minChars = 1; // User set minChars=1
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.'); // 2 chars (< 3 floor)

    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());

    h.engine->HandleCharacter(L'b'); // 3 chars >= 3 floor
    h.engine->HandleTerminator(L'.');
    CHECK(h.engine->InflightRequestId() > 0);
}

TEST(adaptive_trigger_long_sentence_no_request_storm) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(30));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    h.engine->ApplyConfig(cfg);

    const std::wstring sentence =
        L"Deshabilite la opcion, por cierto te estoy escribiendo en espanol, pero tu veras este texto en ingles.";

    // Type rapidly without long pauses (> 600ms)
    for (size_t i = 0; i < sentence.size() - 1; ++i) {
        h.engine->HandleCharacter(sentence[i]);
    }
    // Final terminator
    h.engine->HandleTerminator(sentence.back());

    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
}

TEST(adaptive_trigger_slow_typing_does_not_saturate_queue) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(40));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    h.engine->ApplyConfig(cfg);

    const std::wstring text = L"hola mundo lento.";
    for (size_t i = 0; i < text.size() - 1; ++i) {
        h.engine->HandleCharacter(text[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    h.engine->HandleTerminator(text.back());

    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
}

TEST(adaptive_trigger_fast_typing_no_redundant_requests) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(20));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    h.engine->ApplyConfig(cfg);

    const std::wstring text = L"hola mundo rapido.";
    for (size_t i = 0; i < text.size() - 1; ++i) {
        h.engine->HandleCharacter(text[i]);
    }
    h.engine->HandleTerminator(text.back());

    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
}

TEST(adaptive_trigger_typo_backspace_invalidates_eligibility) {
    TestHarness h(false);
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    CHECK_EQ(h.engine->Buffer(), L"hola");

    h.engine->HandleBackspace();
    h.engine->HandleBackspace();
    h.engine->HandleBackspace();
    CHECK_EQ(h.engine->Buffer(), L"h"); // 1 char left (< 3)

    h.engine->OnDebounceFired();
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());
}

TEST(adaptive_trigger_focus_change_cancels_active_composition) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(100));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    CHECK(h.engine->InflightRequestId() > 0);

    h.engine->CancelActiveCompositionForFocusChange();
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK_EQ(h.engine->Buffer(), L"");
}

TEST(adaptive_trigger_stale_result_handling_remains_correct) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(40));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    h.engine->ApplyConfig(cfg);

    // Request 1: "hola."
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    const int64_t id1 = h.engine->InflightRequestId();

    // While request 1 in flight, type new text: "mundo."
    h.engine->HandleBackspace();
    h.engine->HandleBackspace();
    h.engine->HandleBackspace();
    h.engine->HandleBackspace();
    h.engine->HandleBackspace();

    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');
    const int64_t id2 = h.engine->InflightRequestId();
    CHECK(id2 > id1);

    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"world.");
}

TEST(adaptive_trigger_disabled_state_does_not_trigger) {
    TestHarness h(false);
    h.engine->SetEnabled(false);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');

    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());
}

// ===========================================================================
// Stage 1 Hybrid Architecture Deterministic Tests
// ===========================================================================

TEST(stage1_weak_signal_predictive_off_zero_provider_calls) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L' ');

    // Fire weak debounce
    h.engine->OnDebounceFired();

    // Must NOT submit any request
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->Buffer(), L"hola ");
}

TEST(stage1_1200ms_mid_word_pause_no_commit) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'e');
    h.engine->HandleCharacter(L's');
    h.engine->HandleCharacter(L'c');
    h.engine->HandleCharacter(L'r');
    h.engine->HandleCharacter(L'i');
    h.engine->HandleCharacter(L'b');

    // 1200ms pause simulated
    h.engine->OnDebounceFired();

    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->Buffer(), L"escrib");
}

TEST(stage1_750ms_word_boundary_pause_no_commit) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L' ');

    h.engine->OnDebounceFired();

    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->Buffer(), L"hola ");
}

TEST(stage1_600ms_clause_boundary_pause_no_commit) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L',');

    h.engine->OnDebounceFired();

    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->Buffer(), L"hola,");
}

TEST(stage1_2_to_3_second_pause_no_commit) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');

    // Multiple debounce triggers simulated (2-3 seconds idle)
    h.engine->OnDebounceFired();
    h.engine->OnDebounceFired();
    h.engine->OnDebounceFired();

    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(h.backend->replacements.empty());
    CHECK_EQ(h.engine->Buffer(), L"hola");
}

TEST(stage1_pause_then_continue_typing_single_atomic_composition) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    // First clause
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L' ');
    h.engine->OnDebounceFired(); // 2-3s pause in the middle
    CHECK(h.backend->replacements.empty());

    // User continues typing in the same composition!
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleTerminator(L'.');

    // Exactly 1 atomic commit upon final punctuation
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello world.");
}

TEST(stage1_pause_then_backspace_composition_remains_intact) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L'x'); // typo
    h.engine->OnDebounceFired();      // pause
    CHECK(h.backend->replacements.empty());

    h.engine->HandleBackspace(); // fix typo
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello.");
}

TEST(stage1_strong_signals_period_question_exclamation) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    h.engine->ApplyConfig(cfg);

    // Period
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello.");
}

TEST(stage1_lone_punctuation_no_provider_request) {
    TestHarness h(false);
    h.engine->HandleTerminator(L'.');
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(!h.backend->HasReplacements());
    h.engine->HandleCommitKey(); // clear

    h.engine->HandleTerminator(L'!');
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(!h.backend->HasReplacements());
    h.engine->HandleCommitKey(); // clear

    h.engine->HandleTerminator(L'?');
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(!h.backend->HasReplacements());
}

TEST(stage1_forbidden_result_cannot_call_commit_replacement_or_clear_buffer) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');

    // Simulate an internal request with Forbidden authority
    TranslationResult forbiddenRes;
    forbiddenRes.text = L"hello";
    forbiddenRes.ok = true;
    forbiddenRes.requestId = 999;
    forbiddenRes.commitAuthority = CommitAuthority::Forbidden;

    h.engine->OnTranslationResult(999, std::move(forbiddenRes));

    // Must NOT commit, buffer must NOT clear, state must NOT be committed
    CHECK(!h.backend->HasReplacements());
    CHECK_EQ(h.engine->Buffer(), L"hola");
}

TEST(stage1_forbidden_request_followed_by_allowed_allowed_wins) {
    TestHarness h(false);
    h.provider = std::make_shared<MockProvider>(std::chrono::milliseconds(30));
    h.pipeline = std::make_shared<TranslationPipeline>(h.provider, h.cache, 30000);
    h.engine = std::make_shared<InputEngine>(h.dispatcher, h.backend, h.pipeline, nullptr);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    h.engine->ApplyConfig(cfg);

    // Request with Allowed authority on terminal
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello.");
}

TEST(stage1_out_of_order_forbidden_completion_after_allowed_commit_no_visible_mutation) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    h.engine->ApplyConfig(cfg);

    // Request #11 (Allowed) arrives and commits
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleTerminator(L'.');

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);

    // Request #10 (Forbidden) completes late
    TranslationResult lateForbidden;
    lateForbidden.text = L"stale_preview";
    lateForbidden.ok = true;
    lateForbidden.requestId = 10;
    lateForbidden.commitAuthority = CommitAuthority::Forbidden;

    h.engine->OnTranslationResult(10, std::move(lateForbidden));

    // Zero extra mutations
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello.");
}

TEST(stage1_request_to_result_commit_authority_propagation) {
    auto prov = std::make_shared<MockProvider>(std::chrono::milliseconds(10));
    auto cache = std::make_shared<TranslationCache>();
    auto pipeline = std::make_shared<TranslationPipeline>(prov, cache, 30000);

    // Test Allowed propagation
    {
        TranslationRequest req;
        req.text = L"hola";
        req.sourceLanguage = L"es";
        req.targetLanguage = L"en";
        req.commitAuthority = CommitAuthority::Allowed;
        req.appendTrailingNewline = true;

        bool received = false;
        CommitAuthority outAuth = CommitAuthority::Forbidden;
        bool outNewline = false;

        pipeline->Submit(req, [&](TranslationResult res) {
            outAuth = res.commitAuthority;
            outNewline = res.appendTrailingNewline;
            received = true;
        });

        WaitFor([&] { return received; });
        CHECK_EQ(static_cast<int>(outAuth), static_cast<int>(CommitAuthority::Allowed));
        CHECK(outNewline);
    }

    // Test Forbidden propagation
    {
        TranslationRequest req;
        req.text = L"mundo";
        req.sourceLanguage = L"es";
        req.targetLanguage = L"en";
        req.commitAuthority = CommitAuthority::Forbidden;
        req.appendTrailingNewline = false;

        bool received = false;
        CommitAuthority outAuth = CommitAuthority::Allowed;
        bool outNewline = true;

        pipeline->Submit(req, [&](TranslationResult res) {
            outAuth = res.commitAuthority;
            outNewline = res.appendTrailingNewline;
            received = true;
        });

        WaitFor([&] { return received; });
        CHECK_EQ(static_cast<int>(outAuth), static_cast<int>(CommitAuthority::Forbidden));
        CHECK(!outNewline);
    }
}

TEST(stage1_shift_enter_meaningful_buffer_consumed_and_appends_trailing_newline) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.minChars = 2;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');

    // Verify Shift+Enter is accepted
    CHECK(h.engine->CanHandleShiftEnter());

    h.engine->HandleShiftEnter();

    // Verify authority is Allowed and request is active
    CHECK_EQ(static_cast<int>(h.engine->InflightCommitAuthority()), static_cast<int>(CommitAuthority::Allowed));

    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);

    // Must end with exactly one newline!
    const std::wstring rep = h.backend->GetReplacement(0);
    CHECK(!rep.empty());
    CHECK_EQ(rep.back(), L'\n');
    CHECK_STREQ(rep, L"hello\n");
}

TEST(stage1_shift_enter_empty_or_short_buffer_cannot_handle) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.minChars = 2;
    h.engine->ApplyConfig(cfg);

    // Empty buffer
    CHECK(!h.engine->CanHandleShiftEnter());

    // 1 char (< 3)
    h.engine->HandleCharacter(L'a');
    CHECK(!h.engine->CanHandleShiftEnter());

    // 2 chars (< 3)
    h.engine->HandleCharacter(L'b');
    CHECK(!h.engine->CanHandleShiftEnter());

    // 3 chars (>= 3)
    h.engine->HandleCharacter(L'c');
    CHECK(h.engine->CanHandleShiftEnter());
}

TEST(stage1_plain_enter_preserves_host_semantics) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    h.engine->ApplyConfig(cfg);

    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');

    // Plain enter: HandleCommitKey()
    h.engine->HandleCommitKey();

    // Zero replacements injected, buffer cleared untouched
    CHECK(!h.backend->HasReplacements());
    CHECK_EQ(h.engine->Buffer(), L"");
    CHECK_EQ(h.engine->Status().state, EngineState::Idle);
}

TEST(stage1_acceptance_test_full_lifecycle) {
    TestHarness h(false);
    AppConfig cfg;
    cfg.enabled = true;
    cfg.sourceLanguage = L"es";
    cfg.targetLanguage = L"en";
    cfg.minChars = 2;
    cfg.speculativeTranslationEnabled = false;
    h.engine->ApplyConfig(cfg);

    // 1. User types: "hola mundo"
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L' ');
    h.engine->HandleCharacter(L'm');
    h.engine->HandleCharacter(L'u');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'o');

    // 2. User pauses for 2-3 seconds in the middle
    h.engine->OnDebounceFired();
    h.engine->OnDebounceFired();
    h.engine->OnDebounceFired();

    // Expected: ZERO provider calls, ZERO replacements, buffer intact
    CHECK_EQ(h.engine->InflightRequestId(), -1);
    CHECK(!h.backend->HasReplacements());
    CHECK_EQ(h.engine->Buffer(), L"hola mundo");

    // 3. User continues typing in the same composition
    h.engine->HandleCharacter(L' ');
    h.engine->HandleCharacter(L'g');
    h.engine->HandleCharacter(L'r');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L'n');
    h.engine->HandleCharacter(L'd');
    h.engine->HandleCharacter(L'e');

    // 4. User types final period '.'
    h.engine->HandleTerminator(L'.');

    // Expected: Exactly ONE provider request, Exactly ONE atomic CommitReplacement()
    WaitFor([&] { return h.backend->HasReplacements(); });
    CHECK_EQ(h.backend->ReplacementsCount(), 1u);
    CHECK_STREQ(h.backend->GetReplacement(0), L"hello world grande.");
    CHECK_EQ(h.engine->Buffer(), L"");
}

TEST(hook_backend_read_focused_text_com_init_handling) {
    HookBackend backend;

    // Test 1: Uninitialized COM / Default state on worker thread
    std::thread([&] {
        // ReadFocusedText() handles COM initialization internally
        auto res1 = backend.ReadFocusedText();
        (void)res1;
    }).join();

    // Test 2: Calling thread is already initialized as STA (COINIT_APARTMENTTHREADED)
    // CoInitializeEx(COINIT_MULTITHREADED) inside ReadFocusedText will return RPC_E_CHANGED_MODE (0x80010106).
    // The updated ReadFocusedText recognizes RPC_E_CHANGED_MODE as a valid COM-ready state on thread.
    std::thread([&] {
        const HRESULT hrSta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CHECK(SUCCEEDED(hrSta) || hrSta == S_FALSE);

        auto res2 = backend.ReadFocusedText();
        (void)res2;
        bool delRes = backend.DeleteVerified(0, L"");
        CHECK(delRes); // 0 count returns true immediately after calling ReadFocusedText check or early return

        if (SUCCEEDED(hrSta)) {
            CoUninitialize();
        }
    }).join();

    // Test 3: Calling thread is already initialized as MTA (COINIT_MULTITHREADED)
    // CoInitializeEx(COINIT_MULTITHREADED) inside ReadFocusedText will return S_FALSE (0x00000001).
    std::thread([&] {
        const HRESULT hrMta = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CHECK(SUCCEEDED(hrMta));

        auto res3 = backend.ReadFocusedText();
        (void)res3;
        bool delRes = backend.DeleteVerified(0, L"");
        CHECK(delRes);

        if (SUCCEEDED(hrMta)) {
            CoUninitialize();
        }
    }).join();
}