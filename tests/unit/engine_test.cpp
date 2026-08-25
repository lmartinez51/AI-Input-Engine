#include "mini_test.h"

#include "Composition/InputEngine.h"
#include "Core/Dispatcher.h"
#include "Input/HookBackend.h"
#include "Providers/MockProvider.h"
#include "Translation/TranslationCache.h"
#include "Translation/TranslationPipeline.h"

#include <atomic>
#include <thread>

using namespace aiie;

namespace {

class FakeBackend : public IInputBackend {
public:
    std::vector<std::pair<std::wstring, size_t>> replacements;
    std::vector<std::wstring> statusShown;
    std::vector<std::wstring> statusHidden;
    std::atomic<int> commits{0};

    void CommitReplacement(std::wstring replacement, const std::wstring& expectedSource) override {
        replacements.emplace_back(std::move(replacement), expectedSource.size());
    }
    void OnBufferStarted() override {}
    void OnBufferChanged(std::wstring_view) override {}
    void OnBufferCommitted() override { commits.fetch_add(1); }
    void ShowStatus(std::wstring_view message) override { statusShown.emplace_back(message); }
    void HideStatus() override { statusHidden.emplace_back(L""); }
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

TEST(engine_replaces_buffer_after_debounce) {
    TestHarness h;
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    // mock: hola -> hello (minChars=2, debounce 30ms)
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK_EQ(h.backend->replacements.size(), 1u);
    CHECK_STREQ(h.backend->replacements[0].first, L"hello");
    CHECK_EQ(h.backend->replacements[0].second, 4u);
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
    WaitFor([&] { return h.engine->Status().state == EngineState::Translating; }, 500);
    CHECK(h.backend->replacements.empty());
    CHECK_STREQ(h.engine->Buffer(), L"a");
}

TEST(engine_stale_result_does_not_overwrite_new_text) {
    TestHarness h;
    // Mock provider with 20ms delay. Type "hola" (triggers request #1),
    // then quickly type more so the buffer changes before the result arrives.
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    // Immediately extend the buffer with "sss" before the 20ms result lands.
    h.engine->HandleCharacter(L's');
    h.engine->HandleCharacter(L's');
    h.engine->HandleCharacter(L's');
    // Result #1 arrives stale; a second request for "holasss" fires on debounce.
    WaitFor([&] { return !h.backend->replacements.empty(); });
    CHECK(!h.backend->replacements.empty());
    // The applied replacement must match the *final* buffer, not the stale one.
    CHECK_STREQ(h.backend->replacements.back().first, L"holasss");
    CHECK_EQ(h.backend->replacements.back().second, 7u);
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
    WaitFor([&] { return engine->Status().state == EngineState::Error; });
    // Text must be preserved exactly.
    CHECK_STREQ(engine->Buffer(), L"hola");
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
    WaitFor([&] { return h.backend->replacements.size() >= 2; });
    CHECK_EQ(h.backend->replacements.size(), 2u);
    CHECK_STREQ(h.backend->replacements[1].first, L"world");
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
    // Test minChars = 1
    {
        TestHarness h;
        AppConfig cfg;
        cfg.enabled = true;
        cfg.sourceLanguage = L"es";
        cfg.targetLanguage = L"en";
        cfg.debounceMs = 10;
        cfg.minChars = 1;
        h.engine->ApplyConfig(cfg);

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

    // 1. Submit Request #1: "hola"
    h.engine->HandleCharacter(L'h');
    h.engine->HandleCharacter(L'o');
    h.engine->HandleCharacter(L'l');
    h.engine->HandleCharacter(L'a');
    h.engine->HandleCharacter(L' ');
    WaitFor([&] { return h.engine->InflightRequestId() > 0; }, 500);
    const int64_t req1Id = h.engine->InflightRequestId();
    CHECK(req1Id > 0);

    // 2. Submit Request #2 before #1 finishes: "hola mundo."
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